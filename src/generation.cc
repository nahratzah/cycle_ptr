#include <cycle_ptr/detail/generation.h>

namespace cycle_ptr::detail {


auto generation::gc()
noexcept
-> void {
  if (!gc_flag_.test_and_set(std::memory_order_release))
    gc_();
}

auto generation::fix_ordering(base_control& src, base_control& dst)
noexcept
-> std::shared_lock<std::shared_mutex> {
  auto src_gen = src.generation_.load(),
       dst_gen = dst.generation_.load();
  bool dst_gc_requested = false;

  std::shared_lock<std::shared_mutex> src_merge_lck{ src_gen->merge_mtx_ };
  for (;;) {
    if (src_gen == dst_gen || order_invariant(*src_gen, *dst_gen)) [[unlikely]] {
      while (src_gen != src.generation_) [[unlikely]] {
        src_merge_lck.unlock();
        src_gen = src.generation_.load();
        src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
      }

      if (src_gen == dst_gen || order_invariant(*src_gen, *dst_gen)) [[likely]] {
        break; // break out of for(;;) loop
      }
    }
    src_merge_lck.unlock();

    bool src_gc_requested = false;
    if (src_gen->seq == dst_gen->seq && dst_gen > src_gen) {
      dst_gen.swap(src_gen);
      std::swap(src_gc_requested, dst_gc_requested);
    }

    std::tie(dst_gen, dst_gc_requested) = merge_(
        std::make_tuple(std::move(dst_gen), std::exchange(dst_gc_requested, false)),
        std::make_tuple(src_gen, std::exchange(src_gc_requested, false)));

    // Update dst_gen, in case another merge moved dst away from under us.
    if (dst_gen != dst.generation_) [[unlikely]] {
      if (std::exchange(dst_gc_requested, false)) dst_gen->gc_();
      dst_gen = dst.generation_;
    }

    // Update src_gen, in case another merge moved src away from under us.
    assert(!src_gc_requested);
    assert(!src_merge_lck.owns_lock());
    if (src_gen != src.generation_) [[unlikely]] {
      src_gen = src.generation_.load();
      src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
    } else {
      src_merge_lck.lock();
    }
  }

  // Validate post condition.
  assert(src_gen == src.generation_);
  assert(src_merge_lck.owns_lock() && src_merge_lck.mutex() == &src_gen->merge_mtx_);
  assert(src_gen == dst.generation_.load()
      || order_invariant(*src_gen, *dst.generation_.load()));

  if (dst_gc_requested) dst_gen->gc_();
  return src_merge_lck;
}

auto generation::gc_()
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
    std::lock_guard<std::shared_mutex> lck{ mtx_ };

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
          intrusive_ptr_add_ref(&bc); // ADL

          // Colour change.
          [[maybe_unused]]
          const auto old = bc.store_refs_.exchange(make_refcounter(0u, color::black), std::memory_order_release);
          assert(get_refs(old) == 0u && get_color(old) == color::red);
        });

    // Move to unreachable list, so we can release all GC locks.
    unreachable.splice(unreachable.end(), controls_, reachable_end, controls_.end());
  } // End of lock scope.

  // ----------------------------------------
  // Destruction phase: destroy data in each control block.
  // Clear edges in unreachable pointers.
  std::for_each(
      unreachable.begin(), unreachable.end(),
      [this](base_control& bc) {
        std::lock_guard<std::mutex> lck{ bc.mtx_ }; // Lock edges_
        for (vertex& v : bc.edges_) {
          intrusive_ptr<base_control> dst = v.dst_.exchange(nullptr);
          if (dst != nullptr && dst->generation_ != this)
            dst->release(); // Reference count decrement.
        }
      });

  // Destroy unreachables.
  while (!unreachable.empty()) {
    // Transfer ownership from unreachable list to dedicated pointer.
    const auto bc_ptr = intrusive_ptr<base_control>(
        &unreachable.pop_front(),
        false);

    bc_ptr->clear_data_(); // Object destruction.
  }

  // And we're done. :)
}

auto generation::gc_mark_()
noexcept
-> controls_list::iterator {
  // Create wavefront.
  // Element colors:
  // - WHITE -- strongly reachable.
  // - BLACK -- unreachable.
  // - GREY -- strongly reachable, but referents need color update.
  // - RED -- not strongly reachable, but may or may not be reachable.
  controls_list::iterator wavefront_end = controls_.begin();

  controls_list::iterator i = controls_.begin();
  while (i != controls_.end()) {
    std::uintptr_t expect = make_refcounter(0u, color::white);
    for (;;) {
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
      } else if (get_color(expect) == color::red) {
        ++i;
        break;
      }
    }
  }

  return wavefront_end;
}

auto generation::gc_phase2_mark_(controls_list::iterator b)
noexcept
-> controls_list::iterator {
  controls_list::iterator wavefront_end = b;

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

auto generation::gc_sweep_(controls_list::iterator wavefront_end)
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
      const intrusive_ptr<base_control> dst = edge.dst_.load();

      // We don't need to lock dst->generation_, since it's this generation
      // which is already protected.
      // (When it isn't this generation, we don't process the edge.)
      if (dst == nullptr || dst->generation_ != this)
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
      } while (get_color(expect) != color::white
          && dst->store_refs_.compare_exchange_weak(
              expect,
              make_refcounter(get_refs(expect), color::grey),
              std::memory_order_acq_rel,
              std::memory_order_acquire));
      if (get_color(expect) == color::white)
        continue; // Skip already processed element.

      // Ensure grey node is in wavefront.
      // Yes, we may be moving a node already in the wavefront to a different
      // position.
      // Can't be helped, since we're operating outside red-promotion exclusion.
      assert(get_color(expect) != color::black && get_color(expect) != color::white);
      assert(wavefront_begin != controls_.iterator_to(*dst));

      if (wavefront_end == controls_.iterator_to(*dst)) [[unlikely]] {
        ++wavefront_end;
      } else {
        controls_.splice(wavefront_end, controls_, controls_.iterator_to(*dst));
      }
    }

    ++wavefront_begin;
  }

  return wavefront_begin;
}

auto generation::gc_phase2_sweep_(controls_list::iterator wavefront_end)
noexcept
-> controls_list::iterator {
  for (controls_list::iterator wavefront_begin = controls_.begin();
      wavefront_begin != wavefront_end;
      ++wavefront_begin) {
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
    if (get_color(expect) == color::white) [[likely]] {
      continue; // Already processed in phase 1.
    }

    // Process edges.
    std::lock_guard<std::mutex> bc_lck{ bc.mtx_ };
    for (const vertex& v : bc.edges_) {
      intrusive_ptr<base_control> dst = v.dst_.get();
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
      if (get_color(expect) != color::red) [[likely]] {
        continue; // Already processed or already in wavefront.
      }

      assert(dst != &bc);
      assert(wavefront_end != controls_.end());
      assert(wavefront_begin != controls_.iterator_to(*dst));

      if (wavefront_end == controls_.iterator_to(*dst)) [[unlikely]] {
        ++wavefront_end;
      } else {
        controls_.splice(wavefront_end, controls_, controls_.iterator_to(*dst));
      }
    }
  }

  return wavefront_end;
}

auto generation::merge_(
    std::tuple<intrusive_ptr<generation>, bool> src_tpl,
    std::tuple<intrusive_ptr<generation>, bool> dst_tpl)
noexcept
-> std::tuple<intrusive_ptr<generation>, bool> {
  // Convenience aliases, must be references because of
  // recursive invocation of this function.
  intrusive_ptr<generation>& src = std::get<0>(src_tpl);
  intrusive_ptr<generation>& dst = std::get<0>(dst_tpl);

  // Arguments assertion.
  assert(src != dst && src != nullptr && dst != nullptr);
  assert(order_invariant(*src, *dst)
      || (src->seq == dst->seq && src < dst));

  // Convenience of GC promise booleans.
  bool src_gc_requested = std::get<1>(src_tpl);

  // Lock out GC, controls_ modifications, and merges in src.
  // (We use unique_lock instead of lock_guard, to validate
  // correctness at call to merge0_.)
  const std::unique_lock<std::shared_mutex> src_merge_lck{ src->merge_mtx_ };
  const std::unique_lock<std::shared_mutex> src_lck{ src->mtx_ };

  // Cascade merge operation into edges.
  for (base_control& bc : src->controls_) {
    std::lock_guard<std::mutex> edge_lck{ bc.mtx_ };
    for (const vertex& edge : bc.edges_) {
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
            std::make_tuple(edge_dst_gen, false),
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

auto generation::merge0_(
    std::tuple<generation*, bool> x,
    std::tuple<generation*, bool> y,
    [[maybe_unused]] const std::unique_lock<std::shared_mutex>& x_mtx_lck,
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
  // We lock dst now, as our predicates check for dst to be valid.
  assert(x_mtx_lck.owns_lock() && x_mtx_lck.mutex() == &src->mtx_);
  std::lock_guard<std::shared_mutex> dst_lck{ dst->mtx_ };

  // Stage 1: Update edge reference counters.
  for (base_control& bc : src->controls_) {
    std::lock_guard<std::mutex> edge_lck{ bc.mtx_ };
    for (const vertex& edge : bc.edges_) {
      const auto edge_dst = edge.dst_.get();
      assert(edge_dst == nullptr
          || edge_dst->generation_ == src
          || edge_dst->generation_ == dst
          || order_invariant(*dst, *edge_dst->generation_.load()));

      // Update reference counters.
      // (This predicate is why stage 2 must happen after stage 1.)
      if (edge_dst != nullptr && edge_dst->generation_ == dst)
        edge_dst->release(true);
    }
  }
  // Stage 2: switch generation pointers.
  // Note that we can't combine stage 1 and stage 2,
  // as that could cause incorrect detection of cases where
  // release is to be invoked, leading to too many releases.
  for (base_control& bc : src->controls_) {
    assert(bc.generation_ == src);
    bc.generation_ = intrusive_ptr<generation>(dst, true);
  }

  // Splice onto dst.
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


} /* namespace cycle_ptr::detail */
