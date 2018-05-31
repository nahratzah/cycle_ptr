#ifndef CYCLE_PTR_DETAIL_GENERATION_H
#define CYCLE_PTR_DETAIL_GENERATION_H

#include <atomic>
#include <cassrt>
#include <cstdint>
#include <utility>
#include <cycle_ptr/detail/color.h>
#include <cycle_ptr/detail/base_control.h>

namespace cycle_ptr::detail {


class generation {
  friend auto intrusive_ptr_add_ref(generation* g)
  noexcept
  -> void {
    [[maybe_unused]]
    std::uintptr_t old = g.refs_.fetch_add(1u, std::memory_order_acquire);
    assert(old < UINTPTR_MAX);
  }

  friend auto intrusive_ptr_release(generation* g)
  noexcept
  -> void {
    std::uintptr_t old = refs_.fetch_sub(1u, std::memory_order_release);
    assert(old > 0u);

    if (old == 1u) delete g;
  }

 private:
  using controls_list = llist<base_control, base_control>;

  generation() noexcept = default;

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
  -> boost::intrusive_ptr<generation> {
    return boost::intrusive_ptr<generation>(new generation());
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

  auto gc()
  noexcept
  -> void {
    if (!gc_flag_.test_and_set(std::memory_order_release))
      gc_();
  }

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
  auto fix_ordering(base_control& src, base_control& dst)
  noexcept
  -> std::unique_lock<std::shared_mutex> {
    auto src_gen = src.generation_.load(),
         dst_gen = dst.generation_.load();
    bool src_gc_requested = false,
         dst_gc_requested = false;

    std::unique_lock<std::shared_mutex> src_merge_lck{ src_gen->merge_mtx_ };
    while (src_gen != src.generation_.load()) {
      src_merge_lck.unlock();
      src_gen = src.generation_.load();
      src_merge_lck = std::unique_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
    }

    [[likely]] // Since caller only calls this function if the invariant doesn't hold.
    if (!order_invariant(*src_gen, *dst_gen)) {
      [[unlikely]]
      while (src_gen->seq == dst_gen->seq && src_gen > dst_gen) {
        src_merge_lck.unlock();
        src_gen.swap(dst_gen);
        std::swap(src_gc_requested, dst_gc_requested);
        src_merge_lck = std::unique_lock<std::shared_mutex>{ src_gen->merge_mtx_ };

        while (src_gen != src.generation_.load()) {
          src_merge_lck.unlock();
          src_gen = src.generation_.load();
          src_merge_lck = std::unique_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
        }
      }

      std::tie(dst_gen, dst_gc_requested) = merge_(
          std::make_tuple(src_gen, std::exchange(src_gc_requested, false)),
          std::make_tuple(dst_gen, std::exchange(dst_gc_requested, false)),
          src_merge_lck);
    }

    assert(!src_gc_requested);
    if (dst_gc_requested) dst_gen->gc_();
    return src_merge_lck;
  }

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
  auto gc_()
  noexcept
  -> void {
    controls_list unreachable;

    // Lock scope.
    {
      // ----------------------------------------
      // Locks for phase 1:
      // mtx_ grants write access to controls_ (needed for partitioning mark-sweep algorithm).
      // mtx_ also locks out concurrent GCs, doubling as the critical section for a GC.
      // mtx_ also protects this generation against merging, as that is a controls_ mutating operation.
      //
      // Note that we don't yet block weak red-promotion.
      // All reads on weak pointers, will act as if they happened-before the GC
      // ran and thus as if they happened before the last reference to their
      // data went away.
      std::lock_guard<std::mutex> lck{ mtx_ };

      // Clear GC request flag, signalling that GC has started.
      // (We do this after acquiring initial locks, so that multiple threads can
      // forego their GC invocation.)
      gc_flag_.clear(std::memory_order_acq_rel);

      // Prepare (mark phase).
      controls_list::iterator wavefront_end = gc_mark_();
      if (wavefront_end == controls_.end()) return; // Everything is reachable.

      // Sweep phase.
      controls_list::iterator sweep_end = gc_sweep_(std::move(wavefront_end));
      if (sweep_end == controls_.end()) return; // Everything is reachable.

      // ----------------------------------------
      // Locks for phase 2:
      // exclusive lock on red_promotion_mtx_, prevents weak red-promotions.
      std::lock_guard<std::shared_mutex> red_promotion_lck{ red_promotion_mtx_ };

      // Process marks for phase 2.
      // Ensures that all grey elements in sweep_end, controls_.end() are moved into the wave front.
      wavefront_end = gc_phase2_mark_(std::move(sweep_end));
      if (wavefront_end == controls_.end()) return; // Everything is reachable.

      // Perform phase 2 sweep.
      controls_list::iterator reachable_end = gc_phase2_sweep_(std::move(wavefront_end));
      if (reachable_end == controls_.end()) return; // Everything is reachable.

      // ----------------------------------------
      // Phase 3: mark unreachables black and add a reference to their controls.
      // The range reachable_end, controls_.end(), contains all unreachable elements.
      std::for_each(
          reachable_end, controls_.end(),
          [](base_control& bc) {
            // Acquire ownership of control blocks for unreachable list.
            [[maybe_unused]]
            const auto old_refcount = bc.control_refs_.fetch_add(1u, std::memory_order_acquire);
            assert(old_refcount > 0u && old_refcount < UINTPTR_MAX);

            // Colour change.
            [[maybe_unused]]
            const auto old = bc.store_refs_.exchange(make_refcounter(0u, color::black), std::memory_order_release);
            assert(get_refs(old) == 0u && get_color(old) == color::red);
          });

      // Move to unreachable list, so we can release all GC locks.
      unreachable.splice(controls, reachable_end, controls_.end());
    } // End of lock scope.

    // ----------------------------------------
    // Destruction phase: destroy data in each control block.
    // Clear edges in unreachable pointers.
    std::for_each(
        unreachable.begin(), unreachable.end(),
        [](base_control& bc) {
          std::lock_guard<std::mutex> lck{ bc.mtx_ }; // Lock edges_
          for (vertex& v : edges_) {
            boost::intrusive_ptr<base_control> dst = v.dst_.exchange(nullptr);
            if (dst != nullptr && dst->generation_ != this)
              remove_edge(nullptr, dst); // Reference count decrement.
          }
        });

    // Destroy unreachables.
    while (!unreachable.empty()) {
      // Transfer ownership from unreachable list to dedicated pointer.
      const auto bc_ptr = boost::intrusive_ptr<base_control>(
          &unreachable.pop_front(),
          false);

      bc_ptr->clear_data_(); // Object destruction.
    }

    // And we're done. :)
  }

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
  auto gc_mark_()
  noexcept
  -> controls_list::iterator {
    // Create wavefront.
    // Element colors:
    // - WHITE -- strongly reachable.
    // - BLACK -- unreachable.
    // - GREY -- strongly reachable, but referents need color update.
    // - RED -- not strongly reachable, but may or may not be reachable.
    iterator wavefront_end = controls_.begin();

    iterator i = controls_.begin();
    while (i != controls_.end()) {
      std::uintptr_t expect = make_refcounter(0u, color::white);
      while (get_color(expect) != color::red) {
        assert(get_color(expect) != color::black);
        const color target_color = (get_refs(expect) == 0u ? color::red : color::grey);
        if (i->store_refs_.compare_exchange_weak(
                expect,
                make_refcounter(get_refs(expect), target_color),
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          if (target_color == color::red) {
            ++i;
          } else if (i == wavefront_end) {
            ++wavefront_end;
            ++i;
          } else {
            controls_.splice(wavefront_end, controls_, i++);
          }
          break;
        }
      }
    }

    return wavefront_end;
  }

  /**
   * \brief Phase 2 mark
   * \details
   * Extends the wavefront in ``[controls_.begin(), b)`` with anything
   * non-red after \p b.
   */
  auto gc_phase2_mark_(controls_list::iterator b)
  noexcept
  -> controls_list::iterator {
    iterator wavefront_end = b;

    while (b != controls_.end()) {
      const color b_color =
          get_color(b->store_refs_.load(std::memory_order_acquire));
      assert(b_color == color::grey || b_color == color::red);

      if (b_color == color::red) {
        ++b;
      } else if (b == wavefront_end) {
        ++wavefront_end;
        ++b;
      } else {
        controls_.splice(wavefront_end, controls_, b++);
      }
    }

    return wavefront_end;
  }

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
  auto gc_sweep_(controls_list::iterator wavefront_end)
  noexcept
  -> controls_list::iterator {
    controls_list::iterator wavefront_begin = controls_.begin();

    while (wavefront_begin != wavefront_end) {
      {
        // Promote grey to white.
        // Note that if the colour isn't grey, another thread may have performed
        // red-demotion on this entry and will be queueing for GC.
        std::uintptr_t expect = wavefront_begin->store_refs_.load(std::memory_order_relaxed);
        for (;;) {
          assert(get_color(expect) == color::grey || get_color(expect) == color::red);
          if (wavefront_begin->store_refs_.compare_exchange_weak(
                  expect,
                  make_refcounter(get_refs(expect), color::white),
                  std::memory_order_relaxed,
                  std::memory_order_relaxed))
            break;
        }
      }

      // Lock wavefront_begin->edges_, for processing.
      std::lock_guard<std::mutex> edges_lck{ wavefront_begin->mtx_ };

      for (const vertex& edge : wavefront_begin->edges_) {
        // Note that if dst has this generation, we short circuit the release
        // manually, to prevent recursion.
        //
        // Note that this does not trip a GC, as only edge changes can do that.
        // And release of this pointer simply releases a control, not its
        // associated edge.
        const boost::intrusive_ptr<base_control> dst = edge.dst_.load();

        // We don't need to lock dst->generation_, since it's this generation
        // which is already protected.
        // (When it isn't this generation, we don't process the edge.)
        if (dst->generation_ != this)
          continue;

        // dst color meaning:
        // 1. white -- already processed, skip reprocessing.
        // 2. grey -- either in the wavefront, or marked grey due to red-promotion
        //    (must be moved into wavefront).
        // 3. red -- zero reference count, outside wavefront, requires processing.
        //
        // Only red requires promotion to grey, the other colours remain as is.
        //
        // Note that the current node (*wavefront_begin) is already marked white,
        // thus the handling of that won't mess up anything.
        std::uintptr_t expect = make_refcounter(0, color::red);
        do {
          assert(get_color(expect) != color::black);
        } while (get_color(expect != color::white
                && dst->store_refs_.compare_exchange_weak(
                    expect,
                    make_refcounter(get_refs(expect), color::grey),
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)));
        if (get_color(expect) == color::white)
          continue; // Skip already processed element.

        // Ensure grey node is in wavefront.
        // Yes, we may be moving a node already in the wavefront to a different
        // position.
        // Can't be helped, since we're operating outside red-promotion exclusion.
        assert(get_color(expect) != color::black && get_color(expect) != color::white);
        assert(wavefront_begin != controls_.iterator_to(dst));

        [[unlikely]]
        if (wavefront_end == controls_.iterator_to(*dst))
          ++wavefront_end;
        else
          controls_.splice(wavefront_end, controls_, controls_.iterator_to(*dst));
      }

      ++wavefront_begin;
    }

    return wavefront_begin;
  }

  /**
   * \brief Perform phase 2 mark-sweep.
   * \details
   * In phase 2, weak red promotion is no longer allowed.
   * \returns End partition iterator of reachable set.
   */
  auto gc_phase2_sweep_(controls_list::iterator wavefront_end)
  noexcept
  -> controls::iterator {
    wavefront_begin = controls_.begin();

    while (wavefront_begin != wavefront_end) {
      base_control& bc = *wavefront_begin;

      // Change bc colour to white.
      std::uintptr_t expect = make_refcounter(0, color::grey);
      while (get_color(expect) != color::white) {
        assert(get_color(expect) == color::grey);
        if (bc.store_refs_.compare_exchange_weak(
                expect,
                make_refcounter(get_refs(expect), color::white),
                std::memory_order_relaxed,
                std::memory_order_relaxed))
          break;
      }
      [[likely]]
      if (get_color(expect) == color::white)
        continue; // Already processed in phase 1.

      // Process edges.
      std::lock_guard<std::mutex> bc_lck{ bc.mtx_ };
      for (const vertex& v : bc.edges_) {
        boost::intrusive_ptr<base_control> dst = v.dst_.get();
        if (dst == nullptr || dst->generation_ != this)
          continue; // Skip edges outside this generation.

        expect = make_refcounter(0, color::red);
        while (get_color(expect) == color::red) {
          if (dst->store_refs_.compare_exchange_weak(
                  expect,
                  make_refcounter(get_refs(expect), color::grey),
                  std::memory_order_relaxed,
                  std::memory_order_relaxed))
            break;
        }
        [[likely]]
        if (get_color(expect) != color::red)
          continue; // Already processed or already in wavefront.

        assert(dst != &bc);
        assert(wavefront_end != controls_.end());
        assert(wavefront_begin != controls_.iterator_to(*dst));

        [[unlikely]]
        if (wavefront_end == controls_.iterator_to(*dst))
          ++wavefront_end;
        else
          controls_.splice(wavefront_end, controls_, controls_.iterator_to(*dst));
      }
    }

    return wavefront_end;
  }

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
      std::tuple<boost::intrusive_ptr<generation>, bool> src_tpl,
      std::tuple<boost::intrusive_ptr<generation>, bool> dst_tpl,
      const std::unique_lock<std::shared_mutex>& src_merge_lck)
  noexcept
  -> std::tuple<boost::intrusive_ptr<generation>, bool> {
    // Convenience aliases, must be references because of
    // recursive invocation of this function.
    boost::intrusive_ptr<generation>& src = std::get<0>(src_tpl);
    boost::intrusive_ptr<generation>& dst = std::get<0>(dst_tpl);

    // Arguments assertion.
    assert(src != dst && src != nullptr && dst != nullptr);
    assert(order_invariant(*src, *dst)
        || (src->seq == dst->seq && src < dst));

    // Convenience of GC promise booleans.
    bool src_gc_requested = std::get<1>(src_tpl);

    // Lock out GC, controls_ modifications, and merges in src.
    // (We use unique_lock instead of lock_guard, to validate
    // correctness at call to merge0_.)
    const std::unique_lock<std::shared_mutex> src_lck{ src->mtx_ };

    // Cascade merge operation into edges.
    for (base_control& bc : src->controls_) {
      std::lock_guard<std::mutex> edge_lck{ bc.mtx_ };
      for (const vertex& edge : edges_) {
        // Move edge.
        // We have to restart this, as other threads may change pointers
        // from under us.
        for (auto edge_dst = edge.dst_.load();
            (edge_dst != nullptr
             && edge_dst->generation_ != src
             && edge_dst->generation_ != dst);
            edge_dst = edge.dst_.load()) {
          // Generation check: we only merge if invariant would
          // break after move of ``bc`` into ``dst``.
          const auto edge_dst_gen = edge_dst->generation_.load();
          if (order_invariant(*dst, *edge_dst_gen)) break;

          // Recursion.
          dst_tpl = merge_(
              std::make_tuple(edge_dst_gen.get(), false),
              std::move(dst_tpl));
        }
      }
    }

    // All edges have been moved into or past dst,
    // unless they're in src.
    // Now, we can place src into dst.
    //
    // We update dst_gc_requested.
    std::get<1>(dst_tpl) = merge0_(
        std::make_tuple(src.get(), src_gc_requested),
        std::make_tuple(dst.get(), std::get<1>(dst_tpl)),
        src_lck,
        src_merge_lck);
    return dst_tpl;
  }

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
      [[maybe_unused]] const std::unique_lock<std::shared_mutex>& x_mtx_lck)
      [[maybe_unused]] const std::unique_lock<std::shared_mutex>& x_merge_mtx_lck)
  noexcept
  -> bool {
    // Convenience of accessing arguments.
    generation* src;
    generation* dst;
    bool src_gc_requested, dst_gc_requested;
    std::tie(src, src_gc_requested, dst, dst_gc_requested) = std::tuple_cat(x, y);
    // Validate ordering.
    assert(src != dst && src != nullptr && dst != nullptr);
    assert(order_invariant(*src, *dst)
        || (src->seq == dst->seq && src < dst));
    assert(x_mtx_lck.owns_lock() && x_mtx_lck.mutex() == &src->mtx_);
    assert(x_merge_mtx_lck.owns_lock() && x_merge_mtx_lck.mutex() == &src->merge_mtx_);

    // We promise a GC, because:
    // 1. it's trivial in src
    // 2. the algorithm requires that dst is GC'd at some point
    //    after the merge.
    // Note that, due to ``x_mtx_lck``, no GC can take place on \p src
    // until we're done.
    if (!src_gc_requested) {
      src_gc_requested = !src->gc_flag_.test_and_set();
    } else {
      assert(src->gc_flag_.test_and_set());
    }

    // Propagate responsibility for GC.
    // If another thread has commited to running GC,
    // we'll use that thread's promise,
    // instead of running one ourselves later.
    //
    // This invocation here is an optimization, to prevent
    // other threads from acquiring the responsibility.
    // (Something that'll reduce lock contention probability
    // on ``dst->mtx_``.)
    if (!dst_gc_requested)
      dst_gc_requested = !dst->gc_flag_.test_and_set();
    else
      assert(dst->gc_flag_.test_and_set());

    // Update everything in src, to be moveable to dst.
    std::lock_guard<std::shared_mutex> src_lck{ src->mtx_ };
    // Stage 1: Update edge reference counters.
    for (const base_control& bc : src->controls_) {
      std::lock_guard<std::mutex> edge_lck{ bc.mtx_ };
      for (const vertex& edge : edges_) {
        const auto edge_dst = edge.dst_.get();
        assert(edge_dst == nullptr
            || edge_dst->generation_ == src
            || edge_dst->generation_.load()->seq == dst->seq
            || order_invariant(*dst, *edge_dst->generation_.load()));

        // Update reference counters.
        // (This predicate is why stage 2 must happen after stage 1.)
        if (edge_dst->generation_ == dst) edge_dst->release(false);
      }
    }
    // Stage 2: switch generation pointers.
    // Note that we can't combine stage 1 and stage 2,
    // as that could cause incorrect detection of cases where
    // release is to be invoked, leading to too many releases.
    for (const base_control& bc : src->controls_) {
      assert(bc.generation_ == src);
      bc.generation_ = boost::intrusive_ptr<generation>(dst);
    }

    // Splice onto dst.
    std::lock_guard<std::shared_mutex> dst_lck{ dst->mtx_ };
    dst->controls_.splice(dst->controls_.end(), src->controls_);

    // Fulfill our promise of src GC.
    if (src_gc_requested) {
      // Since src is now empty, GC on it is trivial.
      // So instead of running it, simply clear the flag
      // (but only if we took responsibility for running it).
      src->gc_flag_.clear();
    }

    // Propagate responsibility for GC.
    // If another thread has commited to running GC,
    // we'll use that thread's promise,
    // instead of running one ourselves later.
    //
    // This invocation is repeated here,
    // because if ``!dst_gc_requested``,
    // the thread that promised the GC may have completed.
    // And this could cause missed GC of src elements.
    if (!dst_gc_requested)
      dst_gc_requested = !dst->gc_flag_.test_and_set();
    else
      assert(dst->gc_flag_.test_and_set());

    return dst_gc_requested;
  }

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
  ///\brief Reference counter for boost::intrusive_ptr.
  std::atomic<std::uintptr_t> refs_{ 0u };
  ///\brief Flag indicating a pending GC.
  std::atomic_flag gc_flag_;
};


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_GENERATION_H */
