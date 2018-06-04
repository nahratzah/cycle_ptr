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


/**
 * \brief Base class for all control blocks.
 * \details
 * Contains all variables required for the algorithm to function.
 */
class base_control
: public link<base_control>
{
  friend class generation;
  friend class vertex;
  template<typename> friend class cycle_ptr::cycle_allocator;

  ///\brief Increment reference counter.
  ///\details Used by intrusive_ptr.
  friend auto intrusive_ptr_add_ref(base_control* bc)
  noexcept
  -> void {
    assert(bc != nullptr);

    [[maybe_unused]]
    std::uintptr_t old = bc->control_refs_.fetch_add(1u, std::memory_order_acquire);
    assert(old > 0u && old < UINTPTR_MAX);
  }

  ///\brief Decrement reference counter.
  ///\details Used by intrusive_ptr.
  ///Destroys this if the last reference goes away.
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

  ///\brief Default constructor allocates a new generation.
  base_control();
  ///\brief Constructor to use a specific generation.
  base_control(intrusive_ptr<generation> g) noexcept;
  ///\brief Destructor.
  ~base_control() noexcept;

 public:
  ///\brief Create a control block that represents no ownership.
  static auto unowned_control() -> intrusive_ptr<base_control>;

  ///\brief Test if the object managed by this control is expired.
  auto expired() const
  noexcept
  -> bool {
    return get_color(store_refs_.load(std::memory_order_relaxed)) == color::black;
  }

  ///\brief Implements publisher lookup based on address range.
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

  ///\brief Register a vertex.
  auto push_back(vertex& v)
  noexcept
  -> void {
    std::lock_guard<std::mutex> lck{ mtx_ };
    edges_.push_back(v);
  }

  ///\brief Deregister a vertex.
  auto erase(vertex& v)
  noexcept
  -> void {
    std::lock_guard<std::mutex> lck{ mtx_ };
    edges_.erase(edges_.iterator_to(v));
  }

 private:
  ///\brief Destroy object managed by this control block.
  virtual auto clear_data_() noexcept -> void = 0;
  /**
   * \brief Retrieve the deleter function.
   * \details
   * Deleter is a free function, because control blocks need to use their
   * construction allocator to erase.
   */
  virtual auto get_deleter_() const noexcept -> void (*)(base_control*) noexcept = 0;

  ///\brief Reference counter on managed object.
  ///\details Initially has a value of 1.
  std::atomic<std::uintptr_t> store_refs_{ make_refcounter(1u, color::white) };
  ///\brief Reference counter on control block.
  ///\details Initially has a value of 1.
  std::atomic<std::uintptr_t> control_refs_{ std::uintptr_t(1) };
  ///\brief Pointer to generation.
  hazard_ptr<generation> generation_;
  ///\brief Mutex to protect edges.
  std::mutex mtx_;
  ///\brief List of edges originating from object managed by this control block.
  llist<vertex, vertex> edges_;

 public:
  /**
   * \brief This variable indicates the managed object is under construction.
   * \details
   * It is used to prevent \ref base_control::shared_from_this from
   * handing out references until construction has completed.
   *
   * If a pointer was handed out before, we would get into a difficult scenario
   * where the constructor of an object could publish itself and then fail
   * its construction later.
   * At which point we would have the difficult question of how to manage
   * a dangling pointer.
   *
   * By preventing this, we prevent failed constructors from accidentally
   * publishing an uninitialized pointer.
   */
  bool under_construction = true;
};


/**
 * \brief Address range publisher.
 * \details
 * Publishes that a range of memory is managed by a specific control block,
 * so that \ref base_control and \ref cycle_member_ptr can use automatic
 * deduction of ownership.
 */
class base_control::publisher {
 private:
  ///\brief Address range.
  struct address_range {
    ///\brief Base memory address.
    void* addr;
    ///\brief Size of memory in bytes.
    std::size_t len;

    ///\brief Equality comparison.
    auto operator==(const address_range& other) const
    noexcept
    -> bool {
      return std::tie(addr, len) == std::tie(other.addr, other.len);
    }

    ///\brief Inequality comparison.
    auto operator!=(const address_range& other) const
    noexcept
    -> bool {
      return !(*this == other);
    }

    ///\brief Less operator for use in map.
    auto operator<(const address_range& other) const
    noexcept
    -> bool {
      return addr < other.addr;
    }
  };

  /**
   * \brief Map of address ranges.
   * \details
   * This is an ordered map, sorted by address (ascending).
   *
   * It must be an ordered map, since the lookup queries will likely contain
   * sub-ranges.
   *
   * \note
   * We use a global map for published memory ranges, rather than a TLS variable.
   * \par
   * This is because a TLS variable would break, if the called constructor,
   * during our publishing stage, would jump threads.
   * This is very possible to happen both with boost::asio and the upcoming
   * co-routine additions in C++20.
   * For example, if a co-routine creates an object, which during its
   * construction invokes an awaitable function.
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

  ///\brief Iterator into published data.
  map_type::const_iterator iter_;
};


inline auto base_control::publisher_lookup(void* addr, std::size_t len)
-> intrusive_ptr<base_control> {
  return publisher::lookup(addr, len);
}


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_BASE_CONTROL_H */
