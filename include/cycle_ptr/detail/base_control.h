#ifndef CYCLE_PTR_DETAIL_BASE_CONTROL_H
#define CYCLE_PTR_DETAIL_BASE_CONTROL_H

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cycle_ptr/detail/color.h>
#include <cycle_ptr/detail/hazard.h>
#include <cycle_ptr/detail/llist.h>
#include <cycle_ptr/detail/vertex.h>

namespace cycle_ptr::detail {


class base_control
: public link<base_control>
{
  friend class generation;

  friend auto intrusive_ptr_add_ref(base_control* bc)
  noexcept
  -> void {
    [[maybe_unused]]
    std::uintptr_t old = bc->control_refs_.fetch_add(1u, std::memory_order_acquire);
    assert(old > 0u && old < UINTPTR_MAX);
  }

  friend auto intrusive_ptr_release(base_control* bc)
  noexcept
  -> void {
    std::uintptr_t old = control_refs_.fetch_sub(1u, std::memory_order_release);
    assert(old > 0u);

    if (old == 1u) std::invoke(bc->get_deleter_(), bc);
  }

  base_control(const base_control&) = delete;

 protected:
  class publisher;

  base_control() noexcept {
    auto g = generation::new_generation();
    g->link(*this);
    generation_.reset(std::move(g));
  }

  virtual ~base_control() noexcept {}

 public:
  auto expired() const
  noexcept
  -> bool {
    return get_color(store_refs_.load(std::memory_order_relaxed) != color::black);
  }

  ///\brief Used by weak to strong reference promotion.
  ///\return True if promotion succeeded, false otherwise.
  auto weak_acquire()
  noexcept
  -> bool {
    boost::intrusive_ptr<generation> gen_ptr;
    std::shared_lock<std::shared_mutex> lck;

    std::uintptr_t expect = make_refcounter(1, color::white);
    while (get_color(expect) != color::black) {
      [[unlikely]]
      if (get_color(expect) == color::red && !lck.owns_lock()) {
        // Acquire weak red-promotion lock.
        gen_ptr = generation_.get();
        for (;;) {
          lck = std::shared_lock<std::shared_mutex>(gen_ptr->red_promotion_mtx_);
          if (gen_ptr == generation_) break;
          lck.unlock();
          gen_ptr = generation_;
        }
      }

      const color target_color = (get_color(expect) == color::red
          ? color::grey
          : get_color(expect));
      [[likely]]
      if (store_refs_.compare_exchange_weak(
              expect,
              make_refcounter(get_refs(expect) + 1u, target_color),
              std::memory_order_relaxed,
              std::memory_order_relaxed))
        return true;
    }

    return false;
  }

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
  auto acquire()
  noexcept
  -> void {
    std::uintptr_t expect = make_refcounter(1, color::white);
    for (;;) {
      assert(get_color(expect) != color::black);

      const color target_color = (get_color(expect) == color::red
          ? color::grey
          : get_color(expect));
      [[likely]]
      if (store_refs_.compare_exchange_weak(
              expect,
              make_refcounter(get_refs(expect) + 1u, target_color),
              std::memory_order_relaxed,
              std::memory_order_relaxed))
        return;
    }
  }

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
  auto gc()
  noexcept
  -> void {
    boost::intrusive_ptr<generation> gen_ptr;
    do {
      gen_ptr = generation_.get();
      gen_ptr->gc();
    } while (gen_ptr != generation_);
  }

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

  std::atomic<std::uintptr_t> store_refs_{ (std::uintptr_t(1) << color_shift) | static_cast<std::uintptr_t>(color::white) };
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
    std::size_t* len;

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
  publisher(void* addr, std::size_t len, base_control& bc) {
    const auto mtx_and_map = singleton_map_();
    std::lock_guard<std::shared_mutex> lck{ std::get<std::shared_mutex>(mtx_and_map) };

    [[maybe_unused]]
    bool success;
    std::tie(iter_, success) =
        std::get<map_type>(mtx_and_map).emplace(address_range(addr, len), &bc);

    assert(success);
  }

  ///\brief Destructor, unpublishes the range.
  ~publisher() noexcept {
    const auto mtx_and_map = singleton_map_();
    std::lock_guard<std::shared_mutex> lck{ std::get<std::shared_mutex>(mtx_and_map) };

    std::get<map_type>(mtx_and_map).erase(iter_);
  }

  ///\brief Perform a lookup, to figure out which control manages the given address range.
  ///\details Finds the base_control for which a publisher is active.
  ///The range is usually smaller than the range managed by the control.
  ///\param[in] addr Object offset for which to find a base_control.
  ///\param[in] len Sizeof the object for which to find a base control.
  ///\returns Base control owning the argument address range.
  ///\throws std::runtime_error if no pushlished range covers the argument range.
  static lookup(void* addr, std::size_t len)
  noexcept
  -> base_control& {
    const auto mtx_and_map = singleton_map_();
    std::shared_lock<std::shared_mutex> lck{ std::get<std::shared_mutex>(mtx_and_map) };

    // Find address range after argument range.
    const map_type& map = std::get<map_type>(mtx_and_map);
    auto pos = map.upper_bound(address_range(addr, len));
    assert(pos == map.end() || pos->first.addr > addr);

    // Skip back one position, to find highest address range containing addr.
    [[unlikely]]
    if (pos == map.begin())
      throw std::runtime_error("cycle_ptr: no published control block for given address range.");
    else
      --pos;
    assert(pos != map.end() && pos->first.addr <= addr);
    assert(pos->second != nullptr);

    // Verify if range fits.
    [[likely]]
    if (static_cast<std::uintptr_t>(pos->first.addr) + pos->first.len
        >= static_cast<std::uintptr_t>(addr) + len)
      return *pos->second;

    throw std::runtime_error("cycle_ptr: no published control block for given address range.");
  }

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
  static auto singleton_map_()
  noexcept
  -> std::tuple<std::shared_mutex&, map_type&> {
    static std::shared_mutex mtx;
    static map_type map;
    return std::tie(mtx, map);
  }

  map_type::const_iterator iter_;
};


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_BASE_CONTROL_H */
