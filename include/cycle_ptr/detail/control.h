#ifndef CYCLE_PTR_DETAIL_CONTROL_H
#define CYCLE_PTR_DETAIL_CONTROL_H

#include <cassert>
#include <cstdint>
#include <cycle_ptr/detail/base_control.h>
#include <cycle_ptr/detail/generation.h>

namespace cycle_ptr::detail {


template<typename T, typename Alloc>
class control final
: public base_control,
  private Alloc
{
  using alloc_traits = std::allocator_traits<Alloc>;
  using control_alloc_t = typename alloc_traits<Alloc>::template rebind_alloc<control>;
  using control_alloc_traits_t = typename alloc_traits<Alloc>::template rebind_traits<control>;

  static_assert(std::is_same_v<typename alloc_traits::value_type, T>,
      "Alloc must be allocator of T.");

  control(const control&) = delete;

 private:
  ~control() noexcept = default;

 public:
  control(Alloc alloc) noexcept
  : Alloc(std::move(alloc))
  {}

  template<typename... Args>
  auto instantiate_(Args&&... args)
  -> void {
    try {
      publisher pub{ reinterpret_cast<void*>(&store_), sizeof(store_), *this };
      new (reinterpret_cast<void*>(&store_)) T(std::forward<Args>(args)...);
    } catch (...) {
      throw;
    }

    // Clear construction flag after construction completes.
    this->under_construction = false;
  }

 private:
  auto clear_data_()
  noexcept
  -> void override {
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
#ifndef NDEBUG
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
