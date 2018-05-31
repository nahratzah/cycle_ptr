#include <cycle_ptr/detail/vertex.h>
#include <cycle_ptr/detail/base_control.h>
#include <cycle_ptr/detail/generation.h>

namespace cycle_ptr::detail {


vertex::vertex(boost::intrusive_ptr<base_control> bc) noexcept
: bc_(std::move(bc))
{
  assert(bc_ != nullptr);
  bc_->push_back(*this);
}

vertex::~vertex() noexcept {
  if (bc_->expired()) {
    assert(dst_ == nullptr);
  } else {
    assert(this->link<vertex>::linked());
    reset();
    bc_->erase(*this);
  }
}

auto vertex::reset()
-> void {
  throw_if_owner_expired();

  if (dst_ == nullptr) return;

  boost::intrusive_ptr<generation> src_gen = bc_->generation_.load();

  // Lock src generation against merges.
  std::shared_lock<std::shared_mutex> src_merge_lck{ src_gen->merge_mtx_ };
  while (src_gen != bc_->generation_) {
    src_merge_lck.unlock();
    src_gen = bc_->generation_.load();
    src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
  }

  // Clear old dst and replace with nullptr.
  const boost::intrusive_ptr<base_control> old_dst = dst_.exchange(nullptr);
  if (old_dst != nullptr) {
    if (old_dst->generation_ != src_gen) {
      old_dst->release();
    } else {
      // Because store_refs_ may be a zero reference counter, we can't return
      // the pointer.
      const std::uintptr_t refs = old_dst->store_refs_.load(std::memory_order_relaxed);
      if (get_refs(refs) == 0u && get_color(refs) != color::black)
        old_dst->gc();
    }
  }
}

auto vertex::reset(
    boost::intrusive_ptr<base_control> new_dst,
    bool has_reference,
    bool no_red_promotion)
-> void {
  throw_if_owner_expired();

  assert(!has_reference || no_red_promotion);

  // Need to special case this, because below we release ````dst_``.
  if (dst_ == new_dst) {
    if (new_dst != nullptr && has_reference) new_dst->release();
    return;
  }

  // We have to delay reference counter decrement until *after* the
  // new_dst is stored.
  // This boolean is there to remind us to do so, if we're required to.
  bool drop_reference = false;

  // Source generation.
  // (May be updated below, but must have a lifetime that exceeds either lock.)
  boost::intrusive_ptr<generation> src_gen = bc_->generation_.load();

  // Declare, but do not acquire, lock for fix_ordering result.
  std::unique_lock<std::shared_mutex> src_unique_merge_lck;

  // Lock src generation against merges.
  std::shared_lock<std::shared_mutex> src_merge_lck{ src_gen->merge_mtx_ };
  while (src_gen != bc_->generation_) {
    src_merge_lck.unlock();
    src_gen = bc_->generation_.load();
    src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
  }

  // Update ``new_dst`` reference counter for edge.
  if (new_dst == nullptr) {
    /* SKIP */
  } else if (new_dst->generation_ == src_gen) {
    drop_reference = has_reference;
  } else if (generation::order_invariant(*src_gen, *new_dst->generation_.load())) {
    if (!has_reference) {
      if (no_red_promotion)
        new_dst->acquire_no_red();
      else
        new_dst->acquire();
    }
  } else {
    // Reordering of generations needed.
    src_merge_lck.unlock();
    src_unique_merge_lck = generation::fix_ordering(*bc_, *new_dst);
    src_gen = bc_->generation_.load(); // Update, since it may have changed.
    assert(src_unique_merge_lck.owns_lock());
    assert(src_unique_merge_lck.mutex() == &src_gen->merge_mtx_);

    if (new_dst->generation_ != src_gen) {
      // Guaranteed by generation::fix_ordering call.
      assert(generation::order_invariant(*src_gen, *new_dst->generation_.load()));

      // Acquire reference counter.
      if (!has_reference) {
        if (no_red_promotion)
          new_dst->acquire_no_red();
        else
          new_dst->acquire();
      }
    } else {
      // Ensure no reference counter.
      drop_reference = has_reference;
    }
  }

  // Assert what's been acquired so far.
  // If these fail, code above may have corrupted state already.
  assert(src_merge_lck.owns_lock() || src_unique_merge_lck.owns_lock());
  assert(!src_merge_lck.owns_lock()
      || src_merge_lck.mutex() == &src_gen->merge_mtx_);
  assert(!src_unique_merge_lck.owns_lock()
      || src_unique_merge_lck.mutex() == &src_gen->merge_mtx_);
  assert(src_gen == bc_->generation_);

  // Clear old dst and replace with new dst.
  const boost::intrusive_ptr<base_control> old_dst = dst_.exchange(std::move(new_dst));
  if (old_dst != nullptr) {
    if (old_dst->generation_ != src_gen) {
      old_dst->release();
    } else {
      // Because store_refs_ may be a zero reference counter, we can't return
      // the pointer.
      const std::uintptr_t refs = old_dst->store_refs_.load(std::memory_order_relaxed);
      if (get_refs(refs) == 0u && get_color(refs) != color::black)
        old_dst->gc();
    }
  }

  // Finally, decrement the reference counter.
  if (drop_reference) new_dst->release(false);
}

auto vertex::owner_is_expired() const
noexcept
-> bool {
  return bc_->expired();
}

auto vertex::get_control() const
noexcept
-> boost::intrusive_ptr<base_control> {
  return dst_.load();
}


} /* namespace cycle_ptr::detail */
