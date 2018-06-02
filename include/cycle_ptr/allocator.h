#ifndef CYCLE_PTR_ALLOCATOR_H
#define CYCLE_PTR_ALLOCATOR_H

#include <memory>
#include <functional>
#include <utility>
#include <type_traits>
#include <cycle_ptr/detail/base_control.h>

namespace cycle_ptr {


/**
 * \brief Adaptor for collections with member types.
 * \details Member types are owned by the owner supplied at allocator construction.
 * \tparam Nested Underlying allocator.
 */
template<typename Nested>
class cycle_allocator
: public Nested
{
  template<typename> friend class cycle_allocator;

 public:
  using propagate_on_container_copy_assignment = std::false_type;
  using propagate_on_container_move_assignment = std::false_type;
  using propagate_on_container_swap = std::false_type;
  using is_always_equal = std::false_type;

  template<typename T>
  struct rebind {
    using other = cycle_allocator<typename std::allocator_traits<Nested>::template rebind_alloc<T>>;
  };

  template<typename Other>
  cycle_allocator(const cycle_allocator<Other>& other)
  noexcept(std::is_nothrow_constructible_v<Nested, const Other&>)
  : Nested(other),
    control_(other.control_)
  {}

  template<typename... Args, typename = std::enable_if_t<std::is_constructible_v<Nested, Args...>>>
  explicit cycle_allocator(const cycle_base& base, Args&&... args)
  : Nested(std::forward<Args>(args)...),
    control_(base.control_)
  {}

  /**
   * \brief Constructor.
   * \details
   * Publishes the owner control block prior to construction,
   * to allow for members to pick it up automatically.
   *
   * Forwards to construct as implemented by \p Nested.
   */
  template<typename T, typename... Args>
  auto construct(T* ptr, Args&&... args)
  -> void {
    detail::base_control::publisher pub{ ptr, sizeof(T), *control_ };
    std::allocator_traits<Nested>::construct(*this, ptr, std::forward<Args>(args)...);
  }

  template<typename Dummy = void>
  auto select_on_container_copy_construction()
  -> cycle_allocator {
    static_assert(std::is_void_v<Dummy> && false,
        "You must explicitly specify an allocator with owner during copy.");
    return *this;
  }

  auto operator==(const cycle_allocator& other) const
  noexcept(
      std::allocator_traits<Nested>::is_always_equal::value
      || noexcept(std::declval<const Nested&>() == std::declval<const Nested&>()))
  -> bool {
    if constexpr(std::allocator_traits<Nested>::is_always_equal::value) {
      return control_ == other.control_;
    } else {
      return control_ == other.control_
          && std::equal_to<Nested>()(*this, other);
    }
  }

  auto operator!=(const cycle_allocator& other) const
  noexcept(noexcept(*this == other))
  -> bool {
    return !(*this == other);
  }

 private:
  detail::intrusive_ptr<detail::base_control> control_;
};


} /* namespace cycle_ptr */

#endif /* CYCLE_PTR_ALLOCATOR_H */
