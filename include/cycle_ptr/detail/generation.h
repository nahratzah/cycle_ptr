#ifndef CYCLE_PTR_DETAIL_GENERATION_H
#define CYCLE_PTR_DETAIL_GENERATION_H

#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>
#include <cycle_ptr/detail/color.h>
#include <cycle_ptr/detail/base_control.h>
#include <cycle_ptr/detail/intrusive_ptr.h>

namespace cycle_ptr::detail {


class generation {
  friend auto intrusive_ptr_add_ref(generation* g)
  noexcept
  -> void {
    assert(g != nullptr);

    [[maybe_unused]]
    std::uintptr_t old = g->refs_.fetch_add(1u, std::memory_order_relaxed);
    assert(old < UINTPTR_MAX);
  }

  friend auto intrusive_ptr_release(generation* g)
  noexcept
  -> void {
    assert(g != nullptr);

    std::uintptr_t old = g->refs_.fetch_sub(1u, std::memory_order_release);
    assert(old > 0u);

    if (old == 1u) delete g;
  }

 private:
  using controls_list = llist<base_control, base_control>;

  generation() = default;

  generation(const generation&) = delete;

#ifndef NDEBUG
  ~generation() noexcept {
    assert(controls_.empty());
    assert(refs_ == 0u);
  }
#else
  ~generation() noexcept = default;
#endif

 public:
  static auto new_generation()
  -> intrusive_ptr<generation> {
    return intrusive_ptr<generation>(new generation(), true);
  }

  static auto order_invariant(const generation& origin, const generation& dest)
  noexcept
  -> bool {
    return origin.seq < dest.seq;
  }

  auto link(base_control& bc) noexcept
  -> void {
    std::lock_guard<std::shared_mutex> lck{ mtx_ };
    controls_.push_back(bc);
  }

  auto unlink(base_control& bc) noexcept
  -> void {
    std::lock_guard<std::shared_mutex> lck{ mtx_ };
    controls_.erase(controls_.iterator_to(bc));
  }

 private:
  static auto new_seq_()
  noexcept
  -> std::uintmax_t {
    // Sequence number generation.
    static std::atomic<std::uintmax_t> state{ 0u };
    const std::uintmax_t result = state.fetch_add(1u, std::memory_order_relaxed);

    // uintmax_t will be at least 64 bit.
    // If allocating a new generation each nano second,
    // we would run out of sequence numbers after ~584 years.
    assert(result != UINTPTR_MAX); // We ran out of sequence numbers.

    return result;
  }

 public:
  const std::uintmax_t seq = new_seq_();

  auto gc() noexcept -> void;

  /**
   * \brief Ensure src and dst meet constraint, in order to
   * create an edge between them.
   * \details
   * Ensures that either:
   * 1. order_invariant holds between generation in src and dst; or
   * 2. src and dst are in the same generation.
   *
   * \returns A lock to hold while creating the edge.
   */
  static auto fix_ordering(base_control& src, base_control& dst) noexcept
  -> std::unique_lock<std::shared_mutex>;

 private:
  /**
   * \brief Single run of the GC.
   * \details Performs the GC algorithm.
   *
   * The GC runs in two mark-sweep algorithms, in distinct phases.
   * - Phase 1: operates mark-sweep, but does not lock out other threads.
   * - Phase 2: operates mark-sweep on the remainder, locking out other threads attempting weak-pointer acquisition.
   * - Phase 3: colour everything still not reachable black.
   * - Destruction phase: this phase runs after phase 3, with all GC locks unlocked.
   *   It is responsible for destroying the pointees.
   *   It may, consequently, trip GCs on other generations;
   *   hence why we *must* run it unlocked, otherwise we would get
   *   inter generation lock ordering problems.
   */
  auto gc_() noexcept -> void;

  /**
   * \brief Mark phase of the GC algorithm.
   * \details Marks all white nodes red or grey, depending on their reference
   * counter.
   *
   * Partitions controls_ according to the initial mark predicate.
   * \returns Partition end iterator.
   * All elements before the returned iterator are known reachable,
   * but haven't had their edges processed.
   * All elements after the returned iterator may or may not be reachable.
   */
  auto gc_mark_() noexcept -> controls_list::iterator;

  /**
   * \brief Phase 2 mark
   * \details
   * Extends the wavefront in ``[controls_.begin(), b)`` with anything
   * non-red after \p b.
   */
  auto gc_phase2_mark_(controls_list::iterator b) noexcept
  -> controls_list::iterator;

  /**
   * \brief Sweep phase of the GC algorithm.
   * \details Takes the wavefront and for each element,
   * adds all outgoing links to the wavefront.
   *
   * Processed grey elements are marked white.
   * \returns Partition end iterator.
   * Elements before the iterator are known reachable.
   * Elements after are not reachable (but note that red-promotion may make them reachable).
   */
  auto gc_sweep_(controls_list::iterator wavefront_end) noexcept
  -> controls_list::iterator;

  /**
   * \brief Perform phase 2 mark-sweep.
   * \details
   * In phase 2, weak red promotion is no longer allowed.
   * \returns End partition iterator of reachable set.
   */
  auto gc_phase2_sweep_(controls_list::iterator wavefront_end) noexcept
  -> controls_list::iterator;

  /**
   * \brief Merge two generations.
   * \details
   * Pointers in \p src and \p dst must be distinct,
   * and \p dst must not precede \p src.
   *
   * Also, if \p src and \p dst have the same sequence,
   * address of \p src must be before address of \p dst.
   * \param x,y Generations to merge.
   * \param src_merge_lck Lock on ``merge_mtx_`` in \p src.
   * Used for validation only.
   * \returns Pointer to the merged generation.
   */
  static auto merge_(
      std::tuple<intrusive_ptr<generation>, bool> src_tpl,
      std::tuple<intrusive_ptr<generation>, bool> dst_tpl,
      const std::unique_lock<std::shared_mutex>& src_merge_lck) noexcept
  -> std::tuple<intrusive_ptr<generation>, bool>;

  /**
   * \brief Low level merge operation.
   * \details
   * Moves all elements from \p x into \p y, leaving \p x empty.
   *
   * May only be called when no other generations are sequenced between \p x and \p y.
   *
   * The GC promise (second tuple element) of a generation is fulfilled on
   * the generation that is drained of elements, by propagating it
   * the result.
   *
   * \p x must precede \p y.
   *
   * \p x must be locked for controls_, GC (via controls_) and merge_.
   * \param x,y The two generations to merge together, together with
   * predicate indicating if GC is promised by this thread.
   * Note that the caller must have (shared) ownership of both generations.
   * \param x_mtx_lock Lock on \p x, used for validation only.
   * \param x_mtx_lock Lock on merges from \p x, used for validation only.
   * \returns True if \p y needs to be GC'd by caller.
   */
  [[nodiscard]]
  static auto merge0_(
      std::tuple<generation*, bool> x,
      std::tuple<generation*, bool> y,
      [[maybe_unused]] const std::unique_lock<std::shared_mutex>& x_mtx_lck,
      [[maybe_unused]] const std::unique_lock<std::shared_mutex>& x_merge_mtx_lck) noexcept
  -> bool;

 public:
  ///\brief Mutex protecting controls_ and GC.
  std::shared_mutex mtx_;
  ///\brief Mutex protecting merges.
  ///\note ``merge_mtx_`` must be acquired before ``mtx_``.
  std::shared_mutex merge_mtx_;

 private:
  ///\brief All controls that are part of this generation.
  controls_list controls_;

 public:
  ///\brief Lock to control weak red-promotions.
  ///\details
  ///When performing a weak red promotion, this lock must be held for share.
  ///The GC will hold this lock exclusively, to prevent red promotions.
  ///
  ///Note that this lock is not needed when performing a strong red-promotion,
  ///as the promoted element is known reachable, thus the GC would already have
  ///processed it during phase 1.
  std::shared_mutex red_promotion_mtx_;

 private:
  ///\brief Reference counter for intrusive_ptr.
  std::atomic<std::uintptr_t> refs_{ 0u };
  ///\brief Flag indicating a pending GC.
  std::atomic_flag gc_flag_;
};


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_GENERATION_H */
