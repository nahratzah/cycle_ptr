#ifndef CYCLE_PTR_UTIL_H
#define CYCLE_PTR_UTIL_H

#include <functional>
#include <cycle_ptr/detail/intrusive_ptr.h>
#include <cycle_ptr/detail/generation.h>

namespace cycle_ptr {


class gc_operation {
 public:
  constexpr gc_operation() noexcept = default;

  explicit gc_operation(detail::intrusive_ptr<detail::generation> g) noexcept
  : g_(std::move(g))
  {}

  auto operator()() const
  noexcept
  -> void {
    if (g_ != nullptr) g_->gc_();
  }

 private:
  detail::intrusive_ptr<detail::generation> g_;
};

using delay_gc = std::function<void(gc_operation)>;

auto get_delay_gc() -> delay_gc;
auto set_delay_gc(delay_gc f) -> delay_gc;


} /* namespace cycle_ptr */

#endif /* CYCLE_PTR_UTIL_H */
