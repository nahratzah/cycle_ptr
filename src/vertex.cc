#include <cycle_ptr/detail/vertex.h>
#include <cycle_ptr/detail/base_control.h>
#include <cycle_ptr/detail/generation.h>

namespace cycle_ptr::detail {


vertex::vertex()
: vertex(base_control::publisher_lookup(this, sizeof(this)))
{}

vertex::vertex(intrusive_ptr<base_control> bc) noexcept
: bc_(std::move(bc))
{
  assert(bc_ != nullptr);
  bc_->push_back(*this);
}

vertex::~vertex() noexcept {
  if (bc_->expired()) {
    assert(dst_ == nullptr);
  } else {
    reset();
  }

  assert(this->link<vertex>::linked());
  bc_->erase(*this);
}

auto vertex::reset()
-> void {
  throw_if_owner_expired();

  if (dst_ == nullptr) return;

  intrusive_ptr<generation> src_gen = bc_->generation_.load();

  // Lock src generation against merges.
  std::shared_lock<std::shared_mutex> src_merge_lck{ src_gen->merge_mtx_ };
  while (src_gen != bc_->generation_) {
    src_merge_lck.unlock();
    src_gen = bc_->generation_.load();
    src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
  }

  // Clear old dst and replace with nullptr.
  const intrusive_ptr<base_control> old_dst = dst_.exchange(nullptr);
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
    intrusive_ptr<base_control> new_dst,
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
  intrusive_ptr<generation> src_gen = bc_->generation_.load();

  // Lock src generation against merges.
  std::shared_lock<std::shared_mutex> src_merge_lck;
  if (new_dst != nullptr) {
    src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
    while (src_gen != bc_->generation_) {
      src_merge_lck.unlock();
      src_gen = bc_->generation_.load();
      src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
    }
  } else {
    // Maybe merge generations, if required to maintain order invariant.
    src_merge_lck = generation::fix_ordering(*bc_, *new_dst);
    src_gen = bc_->generation_.load(); // Update, since it may have changed.
    assert(src_merge_lck.owns_lock());
    assert(src_merge_lck.mutex() == &src_gen->merge_mtx_);

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
  assert(src_merge_lck.owns_lock()
      && src_merge_lck.mutex() == &src_gen->merge_mtx_);
  assert(src_gen == bc_->generation_);

  // Clear old dst and replace with new dst.
  const intrusive_ptr<base_control> old_dst = dst_.exchange(new_dst);
  bool drop_old_reference = false;
  bool gc_old_reference = false;
  if (old_dst != nullptr) {
    if (old_dst->generation_ != src_gen) {
      drop_old_reference = true;
    } else {
      // Because store_refs_ may be a zero reference counter, we can't return
      // the pointer.
      const std::uintptr_t refs = old_dst->store_refs_.load(std::memory_order_relaxed);
      if (get_refs(refs) == 0u && get_color(refs) != color::black)
        gc_old_reference = true;
    }
  }

  // Release merge lock.
  src_merge_lck.unlock();

  // Finally, decrement the reference counters.
  // We do this outside the lock.
  if (drop_reference) new_dst->release();
  if (drop_old_reference) old_dst->release();
  if (gc_old_reference) old_dst->gc();
}

auto vertex::owner_is_expired() const
noexcept
-> bool {
  return bc_->expired();
}

auto vertex::get_control() const
noexcept
-> intrusive_ptr<base_control> {
  return dst_.load();
}


} /* namespace cycle_ptr::detail */
