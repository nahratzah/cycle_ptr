#include <cycle_ptr/detail/base_control.h>
#include <cycle_ptr/detail/generation.h>

namespace cycle_ptr::detail {


base_control::base_control() {
  auto g = generation::new_generation();
  g->link(*this);
  generation_.reset(std::move(g));
}

base_control::~base_control() noexcept {
  if (under_construction) {
    assert(store_refs_.load() == make_refcounter(1u, color::white));
    assert(this->linked());
    assert(control_refs_.load() == 1u);

    // Manually unlink from generation.
    generation_.load()->unlink(*this);
  } else {
    assert(store_refs_.load() == make_refcounter(0u, color::black));
    assert(!this->linked());
    assert(control_refs_.load() == 0u);
  }

#ifndef NDEBUG
  std::lock_guard<std::mutex> edge_lck{ mtx_ };
  assert(edges_.empty());
#endif
}

auto base_control::weak_acquire()
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

auto base_control::acquire()
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

auto base_control::gc()
noexcept
-> void {
  boost::intrusive_ptr<generation> gen_ptr;
  do {
    gen_ptr = generation_.get();
    gen_ptr->gc();
  } while (gen_ptr != generation_);
}


auto base_control::publisher::singleton_map_()
noexcept
-> std::tuple<std::shared_mutex&, map_type&> {
  static std::shared_mutex mtx;
  static map_type map;
  return std::tie(mtx, map);
}


} /* namespace cycle_ptr::detail */
