#ifndef CYCLE_PTR_DETAIL_HAZARD_H
#define CYCLE_PTR_DETAIL_HAZARD_H

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>
#include <boost/intrusive_ptr.hpp>

namespace cycle_ptr::detail {


/**
 * \brief Hazard pointer algorithm.
 * \details
 * While hazard algorithm is in theory lock-free and wait-free,
 * since we cannot create an infinite amount of hazard global storage,
 * we do have some spinning operations, that may strike when thread
 * contention is high.
 *
 * \tparam T The element type of the pointer.
 */
template<typename T>
class hazard {
 private:
  /**
   * \brief Cache line aligned pointer.
   * \details
   * We align the atomic pointers, to prevent false sharing.
   *
   * Padding is added to make the data type not only cache-line aligned,
   * but also cache-line sized.
   */
#if __cplusplus >= 201703
  struct alignas(std::hardware_destructive_interference_size) data {
    static_assert(sizeof(std::atomic<T*>) < std::hardware_destructive_interference_size,
        "Cycle_ptr did not expect a platform where cache line is less than or equal to a pointer.");

    std::atomic<T*> ptr = nullptr;
    [[maybe_unused]] char pad_[std::hardware_destructive_interference_size - sizeof(std::atomic<T*>)];
  };
#elif defined(__amd64__) || defined(__x86_64__)
  struct alignas(64) data {
    static_assert(sizeof(std::atomic<T*>) < 64,
        "Cycle_ptr did not expect a platform where cache line is less than or equal to a pointer.");

    std::atomic<T*> ptr = nullptr;
    [[maybe_unused]] char pad_[64 - sizeof(std::atomic<T*>)];
  };
#else
# error No fallback for your architecture, sorry.
#endif

  /**
   * \brief List of hazard pointers.
   * \details
   * We make this the size of the most common page size.
   *
   * Coupled with alignment, this will ensure a single TLB entry
   * can cover the entire hazard range.
   */
  using ptr_set = std::array<data, 4096u / sizeof(data)>;

 public:
  using pointer = boost::intrusive_ptr<T>;

  hazard(const hazard&) = delete;

  explicit hazard() noexcept
  : d_(allocate_())
  {}

  ///\brief Load value in ptr.
  ///\returns The value of ptr. Returned value has ownership.
  [[nodiscard]]
  auto operator()(const std::atomic<T*>& ptr)
  noexcept
  -> pointer {
    return (*this)(ptr, ptr.load(std::memory_order_relaxed));
  }

 private:
  ///\brief Load value in ptr.
  ///\returns The value of ptr. Returned value has ownership.
  [[nodiscard]]
  auto operator()(const std::atomic<T*>& ptr, T* target)
  noexcept
  -> pointer {
    for (;;) {
      // Nullptr case is trivial.
      if (target == nullptr) return target;

      // Publish intent to acquire 'target'.
      for (;;) {
        T* expect = nullptr;
        if (d_.ptr.compare_exchange_strong(
                expect,
                target,
                std::memory_order_acquire,
                std::memory_order_relaxed)) [[likely]] {
          break;
        }
      }

      // Check that ptr (still or again) holds 'target'.
      {
        T*const tmp = ptr.load(std::memory_order_acquire);
        if (tmp != target) [[unlikely]] {
          // Clear published value.
          T* expect = target;
          if (!d_.ptr.compare_exchange_strong(
                  expect,
                  nullptr,
                  std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
            // ABA problem:
            // Since we don't know if the granted reference is the
            // pointer that was originally assigned to ptr, or a newly
            // allocated value at the same address, we
            // have no option but to discard it.
            if (ptr.load(std::memory_order_relaxed) == target) [[unlikely]] {
              return pointer(target, false);
            }

            // Another thread granted us a reference.
            release_(target);
          }

          target = tmp; // Update target to newly found value.
          continue; // Restart after cancellation.
        }
      }

      // Current state:
      // 1. intent published
      // 2. intent is valid
      // Thus, 'target' will have life time guarantee.
      acquire_(target);

      T* expect = target;
      if (!d_.ptr.compare_exchange_strong(
              expect,
              nullptr,
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        // Another thread granted us a reference, meaning we have 2.
        // We want only 1.
        release_(target);
      }
      return pointer(target, false);
    }
  }

 public:
  /**
   * \brief Release pointer, granting it to a hazard operation, if possible.
   * \details
   * Releases the pointer in a way that another hazard may be able to take
   * ownership of \p ptr.
   *
   * Not needed in normal operation.
   * Only required during atomic ptr resets.
   *
   * Because we only require this for pointers participating in the hazards
   * (i.e., atomic pointers), we have to fill in all hazards at release.
   *
   * If we wanted to assign to only one, we would require all pointer resets
   * to go through the hazards.
   * Which would be potentially error prone, not to mention cause a lot of
   * overhead which could be entirely avoided in unshared cases.
   */
  static auto release(T*&& ptr)
  noexcept
  -> void {
    if (ptr == nullptr) return; // Nullptr case is trivial.

    bool two_refs = false;
    for (data& d : ptr_set_()) {
      if (!std::exchange(two_refs, true))
        acquire_(ptr);

      T* expect = ptr;
      if (d.ptr.compare_exchange_strong(
              expect,
              nullptr,
              std::memory_order_release,
              std::memory_order_relaxed)) {
        // Granted one reference to active hazard.
        two_refs = false;
      }
    }

    if (std::exchange(two_refs, false)) release_(ptr);
    release_(ptr);
    ptr = nullptr;
  }

  /**
   * \brief Reset the pointer.
   * \details
   * Assigns a nullptr value to \p ptr.
   *
   * Does the correct thing to ensure the life time invariant of hazard
   * is maintained.
   */
  static auto reset(std::atomic<T*>& ptr)
  noexcept
  -> void {
    release(ptr.exchange(nullptr, std::memory_order_release));
  }

  /**
   * \brief Reset the pointer to the given new value.
   * \details
   * Assigns \p new_value to \p ptr.
   *
   * Does the correct thing to ensure the life time invariant of hazard
   * is maintained.
   *
   * \param[in,out] ptr The atomic pointer that is to be assigned to.
   * \param[in] new_value The newly assigned pointer value.
   *  Ownership is transferred to \p ptr.
   */
  static auto reset(std::atomic<T*>& ptr, pointer&& new_value)
  noexcept
  -> void {
    release(ptr.exchange(new_value.detach(), std::memory_order_release));
  }

  /**
   * \brief Reset the pointer to the given new value.
   * \details
   * Assigns \p new_value to \p ptr.
   *
   * Does the correct thing to ensure the life time invariant of hazard
   * is maintained.
   *
   * \param[in,out] ptr The atomic pointer that is to be assigned to.
   * \param[in] new_value The newly assigned pointer value.
   */
  static auto reset(std::atomic<T*>& ptr, const pointer& new_value)
  noexcept
  -> void {
    reset(ptr, pointer(new_value));
  }

  static auto exchange(std::atomic<T*>& ptr, std::nullptr_t new_value)
  noexcept
  -> pointer {
    T*const rv = ptr.exchange(nullptr, std::memory_order_acq_rel);
    release(acquire_(rv)); // Must grant old value to active hazards.
    return pointer(rv, false);
  }

  static auto exchange(std::atomic<T*>& ptr, pointer&& new_value)
  noexcept
  -> pointer {
    T*const rv = ptr.exchange(new_value.detach(), std::memory_order_acq_rel);
    release(acquire_(rv)); // Must grant old value to active hazards.
    return pointer(rv, false);
  }

  static auto exchange(std::atomic<T*>& ptr, const pointer& new_value)
  noexcept
  -> pointer {
    return exchange(ptr, pointer(new_value));
  }

  static auto compare_exchange_weak(std::atomic<T*>& ptr, pointer& expected, pointer desired)
  noexcept
  -> bool {
    T* expect = expected.get();
    if (ptr.compare_exchange_weak(
            expect,
            desired.get(),
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      desired.detach();
      release(expected.get());
      return true;
    }

    expected = hazard()(ptr, expect);
    return false;
  }

  static auto compare_exchange_strong(std::atomic<T*>& ptr, pointer& expected, pointer desired)
  noexcept
  -> bool {
    hazard hz;

    for (;;) {
      T* expect = expected.get();
      if (ptr.compare_exchange_strong(
              expect,
              desired.get(),
              std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        desired.detach();
        release(expected.get());
        return true;
      }

      auto actual = hz(ptr, expect);
      if (expected != actual) {
        expected = std::move(actual);
        return false;
      }
    }

    /* unreachable */
  }

 private:
  // Hazard data structure; aligned to not cross a page boundary,
  // thus limiting the number of TLB entries required for this to one.
  alignas(sizeof(ptr_set)) static inline ptr_set ptr_set_impl_;

  ///\brief Singleton set of pointers.
  static auto ptr_set_()
  noexcept
  -> ptr_set& {
    return ptr_set_impl_;
  }

  ///\brief Allocate a hazard store.
  static auto allocate_()
  noexcept
  -> data& {
    static std::atomic<unsigned int> seq_{ 0u };

    ptr_set& ps = ptr_set_();
    return ps[seq_.fetch_add(1u, std::memory_order_relaxed) % ps.size()];
  }

  ///\brief Acquire a reference to ptr.
  static auto acquire_(T* ptr)
  noexcept
  -> T* {
    using namespace boost;

    // ADL
    if (ptr != nullptr) intrusive_ptr_add_ref(ptr);
    return ptr;
  }

  ///\brief Release a reference to ptr.
  static auto release_(T* ptr)
  noexcept
  -> void {
    using namespace boost;

    // ADL
    if (ptr != nullptr) intrusive_ptr_release(ptr);
  }

  ///\brief Instance used to publish intent.
  data& d_;
};

template<typename T>
class hazard_ptr {
 private:
  using hazard_t = hazard<T>;

 public:
  using element_type = T;
  using pointer = typename hazard_t::pointer;
  using value_type = pointer;

#if __cplusplus >= 201703
  static constexpr bool is_always_lock_free = std::atomic<T*>::is_always_lock_free;
#endif

  auto is_lock_free() const
  noexcept
  -> bool {
    return ptr_.is_lock_free();
  }

  auto is_lock_free() const volatile
  noexcept
  -> bool {
    return ptr_.is_lock_free();
  }

  hazard_ptr() noexcept = default;

  hazard_ptr(const hazard_ptr& p) noexcept
  : hazard_ptr(hazard_t()(p.ptr_))
  {}

  hazard_ptr(hazard_ptr&& p) noexcept
  : ptr_(p.ptr_.exchange(nullptr, std::memory_order_acq_rel))
  {}

  hazard_ptr(pointer&& p) noexcept
  : ptr_(p.detach())
  {}

  hazard_ptr(const pointer& p) noexcept
  : hazard_ptr(pointer(p))
  {}

  ~hazard_ptr() noexcept {
    reset();
  }

  auto operator=(const hazard_ptr& p)
  noexcept
  -> pointer {
    return *this = p.get();
  }

  auto operator=(hazard_ptr&& p)
  noexcept
  -> pointer {
    return *this = p.exchange(nullptr);
  }

  auto operator=(const pointer& p)
  noexcept
  -> pointer {
    reset(p);
    return p;
  }

  auto operator=(pointer&& p)
  noexcept
  -> pointer {
    reset(p);
    return std::move(p);
  }

  auto reset()
  noexcept
  -> void {
    hazard_t::reset(ptr_);
  }

  auto reset([[maybe_unused]] std::nullptr_t nil)
  noexcept
  -> void {
    hazard_t::reset(ptr_);
  }

  auto reset(pointer&& p)
  noexcept
  -> void {
    store(std::move(p));
  }

  auto reset(const pointer& p)
  noexcept
  -> void {
    store(p);
  }

  operator pointer() const noexcept {
    return get();
  }

  [[nodiscard]]
  auto get() const
  noexcept
  -> pointer {
    return load();
  }

  [[nodiscard]]
  auto load() const
  noexcept
  -> pointer {
    return hazard_t()(ptr_);
  }

  auto store(pointer&& p)
  noexcept
  -> void {
    hazard_t::reset(ptr_, std::move(p));
  }

  auto store(const pointer& p)
  noexcept
  -> void {
    hazard_t::reset(ptr_, p);
  }

  [[nodiscard]]
  auto exchange(pointer&& p)
  noexcept
  -> pointer {
    return hazard_t::exchange(ptr_, std::move(p));
  }

  [[nodiscard]]
  auto exchange(const pointer& p)
  noexcept
  -> pointer {
    return hazard_t::exchange(ptr_, p);
  }

  auto compare_exchange_weak(pointer& expected, pointer desired)
  noexcept
  -> bool {
    return hazard_t::compare_exchange_weak(expected, std::move(desired));
  }

  auto compare_exchange_strong(pointer& expected, pointer desired)
  noexcept
  -> bool {
    return hazard_t::compare_exchange_strong(expected, std::move(desired));
  }

  friend auto operator==(const hazard_ptr& x, std::nullptr_t y)
  noexcept
  -> bool {
    return x.ptr_.load(std::memory_order_acquire) == y;
  }

  friend auto operator==(const hazard_ptr& x, std::add_const_t<T>* y)
  noexcept
  -> bool {
    return x.ptr_.load(std::memory_order_acquire) == y;
  }

  template<typename U>
  friend auto operator==(const hazard_ptr& x, const boost::intrusive_ptr<U>& y)
  noexcept
  -> std::enable_if_t<std::is_convertible_v<U*, T*>, bool> {
    return x == y.get();
  }

  friend auto operator==(std::nullptr_t x, const hazard_ptr& y)
  noexcept
  -> bool {
    return y == x;
  }

  friend auto operator==(std::add_const_t<T>* x, const hazard_ptr& y)
  noexcept
  -> bool {
    return y == x;
  }

  template<typename U>
  friend auto operator==(const boost::intrusive_ptr<U>& x, const hazard_ptr& y)
  noexcept
  -> std::enable_if_t<std::is_convertible_v<U*, T*>, bool> {
    return y == x;
  }

  friend auto operator!=(const hazard_ptr& x, std::nullptr_t y)
  noexcept
  -> bool {
    return !(x == y);
  }

  friend auto operator!=(const hazard_ptr& x, std::add_const_t<T>* y)
  noexcept
  -> bool {
    return !(x == y);
  }

  template<typename U>
  friend auto operator!=(const hazard_ptr& x, const boost::intrusive_ptr<U>& y)
  noexcept
  -> std::enable_if_t<std::is_convertible_v<U*, T*>, bool> {
    return !(x == y);
  }

  friend auto operator!=(std::nullptr_t x, const hazard_ptr& y)
  noexcept
  -> bool {
    return !(x == y);
  }

  friend auto operator!=(std::add_const_t<T>* x, const hazard_ptr& y)
  noexcept
  -> bool {
    return !(x == y);
  }

  template<typename U>
  friend auto operator!=(const boost::intrusive_ptr<U>& x, const hazard_ptr& y)
  noexcept
  -> std::enable_if_t<std::is_convertible_v<U*, T*>, bool> {
    return !(x == y);
  }

 private:
  std::atomic<T*> ptr_ = nullptr;
};


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_HAZARD_H */
