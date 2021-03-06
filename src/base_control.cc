#include <cycle_ptr/detail/base_control.h>
#include <cycle_ptr/detail/generation.h>

namespace cycle_ptr::detail {
namespace {


class unowned_control_impl final
: public base_control
{
 public:
  unowned_control_impl()
  : base_control(generation_singleton_())
  {}

  auto is_unowned() const
  noexcept
  -> bool override {
    return true;
  }

  auto clear_data_()
  noexcept
  -> void override {
    // We never leave the under_construction stage,
    // so we should never be asked to delete our pointee.
    assert(false);
  }

  auto get_deleter_() const
  noexcept
  -> void (*)(base_control* bc) noexcept override {
    return &deleter_impl_;
  }

 private:
  static auto deleter_impl_(base_control* bc)
  noexcept
  -> void {
    assert(bc != nullptr);
#ifdef NDEBUG
    unowned_control_impl* ptr = static_cast<unowned_control_impl*>(bc);
#else
    unowned_control_impl* ptr = dynamic_cast<unowned_control_impl*>(bc);
    assert(ptr != nullptr);
#endif

    delete ptr;
  }

  static auto generation_singleton_()
  -> intrusive_ptr<generation> {
    static const intrusive_ptr<generation> impl = generation::new_generation(0);
    return impl;
  }
};


} /* namespace cycle_ptr::detail::<unnamed> */


base_control::base_control()
: base_control(generation::new_generation())
{}

base_control::base_control(intrusive_ptr<generation> g) noexcept {
  assert(g != nullptr);
  g->link(*this);
  generation_.reset(std::move(g));
}

base_control::~base_control() noexcept {
  if (under_construction) {
    assert(store_refs_.load() == make_refcounter(1u, color::white));
    assert(this->linked());

    // Manually unlink from generation.
    generation_.load()->unlink(*this);
  } else {
    assert(store_refs_.load() == make_refcounter(0u, color::black));
    assert(!this->linked());
  }

  assert(control_refs_.load() == 0u);

#ifndef NDEBUG
  std::lock_guard<std::mutex> edge_lck{ mtx_ };
  assert(edges_.empty());
#endif
}

auto base_control::unowned_control()
-> intrusive_ptr<base_control> {
  return intrusive_ptr<base_control>(new unowned_control_impl(), false);
}

auto base_control::weak_acquire()
noexcept
-> bool {
  intrusive_ptr<generation> gen_ptr;
  std::shared_lock<std::shared_mutex> lck;

  std::uintptr_t expect = make_refcounter(1, color::white);
  while (get_color(expect) != color::black) {
    if (get_color(expect) == color::red && !lck.owns_lock()) [[unlikely]] {
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
    if (store_refs_.compare_exchange_weak(
            expect,
            make_refcounter(get_refs(expect) + 1u, target_color),
            std::memory_order_relaxed,
            std::memory_order_relaxed)) [[likely]] {
      return true;
    }
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
    if (store_refs_.compare_exchange_weak(
            expect,
            make_refcounter(get_refs(expect) + 1u, target_color),
            std::memory_order_relaxed,
            std::memory_order_relaxed)) [[likely]] {
      return;
    }
  }
}

auto base_control::gc()
noexcept
-> void {
  intrusive_ptr<generation> gen_ptr;
  do {
    gen_ptr = generation_.get();
    gen_ptr->gc();
  } while (gen_ptr != generation_);
}

auto base_control::is_unowned() const
noexcept
-> bool {
  return false;
}


base_control::publisher::publisher(void* addr, std::size_t len, base_control& bc) {
  const auto mtx_and_map = singleton_map_();
  std::lock_guard<std::shared_mutex> lck{ std::get<std::shared_mutex&>(mtx_and_map) };

  [[maybe_unused]]
  bool success;
  std::tie(iter_, success) =
      std::get<map_type&>(mtx_and_map).emplace(address_range{ addr, len }, &bc);

  assert(success);
}

base_control::publisher::~publisher() noexcept {
  const auto mtx_and_map = singleton_map_();
  std::lock_guard<std::shared_mutex> lck{ std::get<std::shared_mutex&>(mtx_and_map) };

  std::get<map_type&>(mtx_and_map).erase(iter_);
}

auto base_control::publisher::lookup(void* addr, std::size_t len)
-> intrusive_ptr<base_control> {
  const auto mtx_and_map = singleton_map_();
  std::shared_lock<std::shared_mutex> lck{ std::get<std::shared_mutex&>(mtx_and_map) };

  // Find address range after argument range.
  const map_type& map = std::get<map_type&>(mtx_and_map);
  auto pos = map.upper_bound(address_range{ addr, len });
  assert(pos == map.end() || pos->first.addr > addr);

  // Skip back one position, to find highest address range containing addr.
  if (pos == map.begin()) [[unlikely]] {
    throw std::runtime_error("cycle_ptr: no published control block for given address range.");
  } else {
    --pos;
  }
  assert(pos != map.end() && pos->first.addr <= addr);
  assert(pos->second != nullptr);

  // Verify if range fits.
  if (reinterpret_cast<std::uintptr_t>(pos->first.addr) + pos->first.len
      >= reinterpret_cast<std::uintptr_t>(addr) + len) [[likely]] {
    return intrusive_ptr<base_control>(pos->second, true);
  }

  throw std::runtime_error("cycle_ptr: no published control block for given address range.");
}

auto base_control::publisher::singleton_map_()
noexcept
-> std::tuple<std::shared_mutex&, map_type&> {
  static std::shared_mutex mtx;
  static map_type map;
  return std::tie(mtx, map);
}


} /* namespace cycle_ptr::detail */
