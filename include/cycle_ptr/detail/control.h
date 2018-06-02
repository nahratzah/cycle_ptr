#ifndef CYCLE_PTR_DETAIL_CONTROL_H
#define CYCLE_PTR_DETAIL_CONTROL_H

#include <cassert>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <cycle_ptr/detail/base_control.h>
#include <cycle_ptr/detail/generation.h>

namespace cycle_ptr::detail {


template<typename T, typename Alloc>
class control final
: public base_control,
  private Alloc
{
  using alloc_traits = std::allocator_traits<Alloc>;
  using control_alloc_t = typename alloc_traits::template rebind_alloc<control>;
  using control_alloc_traits_t = typename alloc_traits::template rebind_traits<control>;

  static_assert(std::is_same_v<typename alloc_traits::value_type, T>,
      "Alloc must be allocator of T.");

  control(const control&) = delete;

 public:
  control(Alloc alloc)
  : Alloc(std::move(alloc))
  {}

  template<typename... Args>
  auto instantiate(Args&&... args)
  -> T* {
    assert(this->under_construction);

    publisher pub{ reinterpret_cast<void*>(&store_), sizeof(store_), *this };
    new (reinterpret_cast<void*>(&store_)) T(std::forward<Args>(args)...); // May throw.

    // Clear construction flag after construction completes successfully.
    this->under_construction = false;

    return reinterpret_cast<T*>(&store_);
  }

 private:
  auto clear_data_()
  noexcept
  -> void override {
    assert(!this->under_construction);
    reinterpret_cast<T*>(&store_)->~T();
  }

  auto get_deleter_() const
  noexcept
  -> void (*)(base_control* bc) noexcept override {
    return &deleter_impl_;
  }

  static auto deleter_impl_(base_control* bc)
  noexcept
  -> void {
    assert(bc != nullptr);
#ifdef NDEBUG
    control* ptr = static_cast<control*>(bc);
#else
    control* ptr = dynamic_cast<control*>(bc);
    assert(ptr != nullptr);
#endif

    control_alloc_t alloc = std::move(*ptr);
    control_alloc_traits_t::destroy(alloc, ptr);
    control_alloc_traits_t::deallocate(alloc, ptr, 1);
  }

  std::aligned_storage_t<sizeof(T), alignof(T)> store_;
};


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_CONTROL_H */
