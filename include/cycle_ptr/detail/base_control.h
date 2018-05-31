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

  /**
   * \brief Register an edge between src and dst.
   * \details
   * Adds an edge between \p src and \p dst.
   *
   * \param src The origin of the edge.
   * \param dst The destination of the edge. May be null.
   * \param has_reference If set, \p dst has a reference count that will be consumed.
   * (Ignored for null dst.)
   * \param no_red_promotion If set, caller guarantees no red promotion is required.
   * Note that this must be set, if has_reference is set.
   * (Ignored for null dst.)
   */
  static auto set_edge(
      vertex& src,
      boost::intrusive_ptr<base_control> dst,
      bool has_reference,
      bool no_red_promotion)
  noexcept
  -> void {
    assert(!has_reference || no_red_promotion);

    // Need to special case this, because below we release ``src.dst_``.
    if (src.dst_ == dst) {
      if (dst != nullptr && has_reference) dst->release();
      return;
    }

    // Source generation.
    // (May be updated below, but must have a lifetime that exceeds either lock.)
    boost::intrusive_ptr<generation> src_gen = src.bc_->generation_.load();

    // Declare, but do not acquire, lock for fix_ordering result.
    std::unique_lock<std::shared_mutex> src_unique_merge_lck;

    // Lock src generation against merges.
    std::shared_lock<std::shared_mutex> src_merge_lck{ src_gen->merge_mtx_ };
    while (src_gen != src.generation_.load()) {
      src_merge_lck.unlock();
      src_gen = src.generation_.load();
      src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
    }

    // Update dst reference counter for edge.
    if (dst == nullptr) {
      /* SKIP */
    } else if (dst->generation_ == src_gen) {
      if (has_reference) dst->release(false);
    } else if (generation::order_invariant(*src_gen, *dst->generation_.load())) {
      if (!has_reference) {
        if (no_red_promotion)
          dst->acquire_no_red();
        else
          dst->acquire();
      }
    } else {
      // Reordering of generations needed.
      src_merge_lck.unlock();
      src_unique_merge_lck = generation::fix_ordering(*src.bc_, *dst);
      src_gen = src.bc_.generation.load(); // Update, since it may have changed.
      assert(src_unique_merge_lck.owns_lock());
      assert(src_unique_merge_lck.mutex() == &src_gen.merge_mtx_);

      if (dst->generation_ != src_gen) {
        // Guaranteed by generation::fix_ordering call.
        assert(generation::order_invariant(src_gen, dst->generation_.load()));

        // Acquire reference counter.
        if (!has_reference) {
          if (no_red_promotion)
            dst->acquire_no_red();
          else
            dst->acquire();
        }
      } else {
        // Ensure no reference counter.
        if (has_reference) dst->release(false);
      }
    }

    // Assert what's been acquired so far.
    // If these fail, code above may have corrupted state already.
    assert(src_merge_lck.owns_lock() || src_unique_merge_lck.owns_lock());
    assert(!src_merge_lck.owns_lock()
        || src_merge_lck.mutex() == &src_gen->merge_mtx_);
    assert(!src_unique_merge_lck.owns_lock()
        || src_unique_merge_lck.mutex() == &src_gen->merge_mtx_);
    assert(src_gen == src.bc_.generation_);

    // Clear old dst and replace with new dst.
    const boost::intrusive_ptr<base_control> old_dst = src.dst_.exchange(std::move(dst));
    if (old_dst != nullptr) {
      if (old_dst->generation_ != src_gen) {
        release(src.dst_);
      } else {
        // Because store_refs_ may be a zero reference counter, we can't return
        // the pointer.
        const std::uintptr_t refs = old_dst.store_refs_.load(std::memory_order_relaxed);
        if (get_refs(refs) == 0u && get_color(refs) != color::black)
          old_dst->gc();
      }
    }
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
  virtual auto get_deleter_() noexcept -> void (*)(base_control*) noexcept = 0;

  std::atomic<std::uintptr_t> store_refs_{ (std::uintptr_t(1) << color_shift) | static_cast<std::uintptr_t>(color::white) };
  std::atomic<std::uintptr_t> control_refs_{ std::uintptr_t(1) };
  hazard_ptr<generation> generation_;
  std::mutex mtx_; // Protects edges_.
  llist<vertex, vertex> edges_;
};


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_BASE_CONTROL_H */
