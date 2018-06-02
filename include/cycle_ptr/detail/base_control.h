#ifndef CYCLE_PTR_DETAIL_BASE_CONTROL_H
#define CYCLE_PTR_DETAIL_BASE_CONTROL_H

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <cycle_ptr/detail/color.h>
#include <cycle_ptr/detail/hazard.h>
#include <cycle_ptr/detail/llist.h>
#include <cycle_ptr/detail/vertex.h>
#include <cycle_ptr/detail/intrusive_ptr.h>

namespace cycle_ptr {
template<typename> class cycle_allocator;
} /* namespace cycle_ptr */

namespace cycle_ptr::detail {


class base_control
: public link<base_control>
{
  friend class generation;
  friend class vertex;
  template<typename> friend class cycle_ptr::cycle_allocator;

  friend auto intrusive_ptr_add_ref(base_control* bc)
  noexcept
  -> void {
    assert(bc != nullptr);

    [[maybe_unused]]
    std::uintptr_t old = bc->control_refs_.fetch_add(1u, std::memory_order_acquire);
    assert(old > 0u && old < UINTPTR_MAX);
  }

  friend auto intrusive_ptr_release(base_control* bc)
  noexcept
  -> void {
    assert(bc != nullptr);

    std::uintptr_t old = bc->control_refs_.fetch_sub(1u, std::memory_order_release);
    assert(old > 0u);

    if (old == 1u) std::invoke(bc->get_deleter_(), bc);
  }

  base_control(const base_control&) = delete;

 protected:
  class publisher;

  base_control();
  base_control(intrusive_ptr<generation> g) noexcept;
  virtual ~base_control() noexcept;

 public:
  ///\brief Create a control block that represents no ownership.
  static auto unowned_control() -> intrusive_ptr<base_control>;

  ///\brief Test if the object managed by this control is expired.
  auto expired() const
  noexcept
  -> bool {
    return get_color(store_refs_.load(std::memory_order_relaxed)) == color::black;
  }

  static auto publisher_lookup(void* addr, std::size_t len) -> intrusive_ptr<base_control>;

  ///\brief Used by weak to strong reference promotion.
  ///\return True if promotion succeeded, false otherwise.
  auto weak_acquire() noexcept -> bool;

  /**
   * \brief Acquire reference.
   * \details
   * May only be called when it is certain there won't be red promotion
   * involved (meaning, it's certain that the reference counter is
   * greater than zero).
   *
   * Faster than acquire().
   */
  auto acquire_no_red()
  noexcept
  -> void {
    [[maybe_unused]]
    std::uintptr_t old = store_refs_.fetch_add(1u << color_shift, std::memory_order_relaxed);
    assert(get_color(old) != color::black && get_color(old) != color::red);
  }

  /**
   * \brief Acquire reference.
   * \details
   * Increments the reference counter.
   *
   * May only be called on reachable instances of this.
   */
  auto acquire() noexcept -> void;

  /**
   * \brief Release reference counter.
   * \details
   * Reference counter is decremented by one.
   * If the reference counter drops to zero, a GC invocation will be performed,
   * unless \p skip_gc is set.
   *
   * \param skip_gc Flag to suppress GC invocation. May only be set to true if
   * caller can guarantee that this is live.
   */
  auto release(bool skip_gc = false)
  noexcept
  -> void {
    const std::uintptr_t old = store_refs_.fetch_sub(
        1u << color_shift,
        std::memory_order_release);
    assert(get_refs(old) > 0u);

    if (!skip_gc && get_refs(old) == 1u) gc();
  }

  /**
   * \brief Run GC.
   */
  auto gc() noexcept -> void;

  auto push_back(vertex& v)
  noexcept
  -> void {
    std::lock_guard<std::mutex> lck{ mtx_ };
    edges_.push_back(v);
  }

  auto erase(vertex& v)
  noexcept
  -> void {
    std::lock_guard<std::mutex> lck{ mtx_ };
    edges_.erase(edges_.iterator_to(v));
  }

 private:
  virtual auto clear_data_() noexcept -> void = 0;
  virtual auto get_deleter_() const noexcept -> void (*)(base_control*) noexcept = 0;

  std::atomic<std::uintptr_t> store_refs_{ make_refcounter(1u, color::white) };
  std::atomic<std::uintptr_t> control_refs_{ std::uintptr_t(1) };
  hazard_ptr<generation> generation_;
  std::mutex mtx_; // Protects edges_.
  llist<vertex, vertex> edges_;

 public:
  bool under_construction = true;
};


class base_control::publisher {
 private:
  struct address_range {
    void* addr;
    std::size_t len;

    auto operator==(const address_range& other) const
    noexcept
    -> bool {
      return std::tie(addr, len) == std::tie(other.addr, other.len);
    }

    auto operator!=(const address_range& other) const
    noexcept
    -> bool {
      return !(*this == other);
    }

    auto operator<(const address_range& other) const
    noexcept
    -> bool {
      return std::tie(addr, len) < std::tie(other.addr, other.len);
    }
  };

  /**
   * \brief Map of address ranges.
   * \details
   * This is an ordered map, sorted by address (ascending).
   *
   * It must be an ordered map, since the lookup queries will likely contain
   * sub-ranges.
   */
  using map_type = std::map<address_range, base_control*>;

  publisher() = delete;
  publisher(const publisher&) = delete;

 public:
  ///\brief Publish a base_control for an object at the given address.
  publisher(void* addr, std::size_t len, base_control& bc);
  ///\brief Destructor, unpublishes the range.
  ~publisher() noexcept;

  ///\brief Perform a lookup, to figure out which control manages the given address range.
  ///\details Finds the base_control for which a publisher is active.
  ///The range is usually smaller than the range managed by the control.
  ///\param[in] addr Object offset for which to find a base_control.
  ///\param[in] len Sizeof the object for which to find a base control.
  ///\returns Base control owning the argument address range.
  ///\throws std::runtime_error if no pushlished range covers the argument range.
  static auto lookup(void* addr, std::size_t len) -> intrusive_ptr<base_control>;

 private:
  /**
   * \brief Global map of ranges.
   * \details The singleton map maintains all published ranges.
   *
   * It is a map, instead of a TLS pointer, as the latter might be wrong, when
   * an object constructor calls a method implemented as a co-routine.
   * In the case of (boost) asio, this could cause the constructor to switch
   * threads and thus make the pointer invisible.
   *
   * \returns Map for range publication, with its associated mutex.
   */
  static auto singleton_map_() noexcept
  -> std::tuple<std::shared_mutex&, map_type&>;

  map_type::const_iterator iter_;
};


inline auto base_control::publisher_lookup(void* addr, std::size_t len)
-> intrusive_ptr<base_control> {
  return publisher::lookup(addr, len);
}


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_BASE_CONTROL_H */
