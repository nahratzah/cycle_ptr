#ifndef CYCLE_PTR_CYCLE_PTR_H
#define CYCLE_PTR_CYCLE_PTR_H

#include <cassert>
#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <cycle_ptr/detail/intrusive_ptr.h>
#include <cycle_ptr/detail/control.h>
#include <cycle_ptr/detail/vertex.h>

namespace cycle_ptr {


template<typename> class cycle_member_ptr;
template<typename> class cycle_gptr;
template<typename> class cycle_weak_ptr;

template<typename T, typename Alloc, typename... Args>
auto allocate_cycle(Alloc&& alloc, Args&&... args) -> cycle_gptr<T>;

class cycle_base {
  template<typename> friend class cycle_member_ptr;

 protected:
  cycle_base()
  : control_(detail::base_control::publisher_lookup(this, sizeof(this)))
  {}

  cycle_base(const cycle_base&)
  noexcept
  : cycle_base()
  {}

  auto operator=(const cycle_base&)
  noexcept
  -> cycle_base& {
    return *this;
  }

  ~cycle_base() noexcept = default;

  template<typename T>
  auto shared_from_this(T* this_ptr)
  -> cycle_gptr<T> {
    assert(control_ != nullptr);

    // Protect against leaking out this from inside constructors.
    // This mimics std::shared_ptr, where shared_from_this() is not
    // valid until after the construction completes.
    [[unlikely]]
    if (control_->under_construction)
      throw std::bad_weak_ptr();

    cycle_gptr<T> result;
    if (!control_->weak_acquire()) throw std::bad_weak_ptr();
    result.emplace_(this_ptr, control_);
    return result;
  }

 private:
  const detail::intrusive_ptr<detail::base_control> control_;
};

template<typename T>
class cycle_member_ptr
: private detail::vertex
{
  template<typename> friend class cycle_member_ptr;
  template<typename> friend class cycle_gptr;
  template<typename> friend class cycle_weak_ptr;

 public:
  using element_type = std::remove_extent_t<T>;
  using weak_type = cycle_weak_ptr<T>;

  explicit cycle_member_ptr(cycle_base& owner) noexcept
  : vertex(owner.control_)
  {}

  cycle_member_ptr(cycle_base& owner, [[maybe_unused]] std::nullptr_t nil) noexcept
  : cycle_member_ptr(owner)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, const cycle_member_ptr<U>& ptr)
  : detail::vertex(owner.control_),
    target_(ptr.target_)
  {
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, cycle_member_ptr<U>&& ptr)
  : cycle_member_ptr(owner, ptr)
  {
    ptr.reset();
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, const cycle_gptr<U>& ptr)
  : detail::vertex(owner.control_),
    target_(ptr.target_)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, cycle_gptr<U>&& ptr)
  : detail::vertex(owner.control_),
    target_(std::exchange(ptr.target_, nullptr))
  {
    this->detail::vertex::reset(
        std::move(ptr.target_ctrl_),
        true, true);
  }

  template<typename U>
  cycle_member_ptr(cycle_base& owner, const cycle_member_ptr<U>& ptr, element_type* target)
  : detail::vertex(owner.control_),
    target_(target)
  {
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
  }

  template<typename U>
  cycle_member_ptr(cycle_base& owner, const cycle_gptr<U>& ptr, element_type* target)
  : detail::vertex(owner.control_),
    target_(target)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, const cycle_weak_ptr<U>& ptr)
  : cycle_member_ptr(owner, cycle_gptr<U>(ptr))
  {}

  cycle_member_ptr() {}
  cycle_member_ptr([[maybe_unused]] std::nullptr_t nil) {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(const cycle_member_ptr<U>& ptr)
  : target_(ptr.target_)
  {
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_member_ptr<U>&& ptr)
  : cycle_member_ptr(ptr)
  {
    ptr.reset();
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(const cycle_gptr<U>& ptr)
  : target_(ptr.target_)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_gptr<U>&& ptr)
  : target_(std::exchange(ptr.target_, nullptr))
  {
    this->detail::vertex::reset(
        std::move(ptr.target_ctrl_),
        true, true);
  }

  template<typename U>
  cycle_member_ptr(const cycle_member_ptr<U>& ptr, element_type* target)
  : target_(target)
  {
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
  }

  template<typename U>
  cycle_member_ptr(const cycle_gptr<U>& ptr, element_type* target)
  : target_(target)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  explicit cycle_member_ptr(const cycle_weak_ptr<U>& ptr)
  : cycle_member_ptr(cycle_gptr<U>(ptr))
  {}

  auto operator=([[maybe_unused]] std::nullptr_t nil)
  -> cycle_member_ptr& {
    reset();
    return *this;
  }

  auto operator=(const cycle_member_ptr& other)
  -> cycle_member_ptr& {
    other.throw_if_owner_expired();

    target_ = other.target_;
    this->detail::vertex::reset(other.get_control(), false, false);
    return *this;
  }

  auto operator=(cycle_member_ptr&& other)
  -> cycle_member_ptr& {
    *this = other;
    other.reset();
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_member_ptr<U>& other)
  -> cycle_member_ptr& {
    other.throw_if_owner_expired();

    target_ = other.target_;
    this->detail::vertex::reset(other.get_control(), false, false);
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(cycle_member_ptr<U>&& other)
  -> cycle_member_ptr& {
    *this = other;
    other.reset();
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_gptr<U>& other)
  -> cycle_member_ptr& {
    target_ = other.target_;
    this->detail::vertex::reset(other.target_ctrl_, false, true);
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(cycle_gptr<U>&& other)
  -> cycle_member_ptr& {
    target_ = std::exchange(other.target_, nullptr);
    this->detail::vertex::reset(
        std::move(other.target_ctrl_),
        false, true);
    return *this;
  }

  auto reset()
  -> void {
    target_ = nullptr;
    this->detail::vertex::reset();
  }

  auto swap(cycle_member_ptr& other)
  -> void {
    std::tie(*this, other) = std::forward_as_tuple(
        cycle_gptr<T>(std::move(other)),
        cycle_gptr<T>(std::move(*this)));
  }

  auto swap(cycle_gptr<T>& other)
  -> void {
    std::tie(*this, other) = std::forward_as_tuple(
        cycle_gptr<T>(std::move(other)),
        cycle_gptr<T>(std::move(*this)));
  }

  auto get() const
  -> T* {
    throw_if_owner_expired();
    return target_;
  }

  template<bool Enable = !std::is_void_v<T>>
  auto operator*() const
  -> std::enable_if_t<Enable, T>& {
    assert(get() != nullptr);
    return *get();
  }

  template<bool Enable = !std::is_void_v<T>>
  auto operator->() const
  -> std::enable_if_t<Enable, T>* {
    assert(get() != nullptr);
    return get();
  }

  explicit operator bool() const {
    return get() != nullptr;
  }

  template<typename U>
  auto owner_before(const cycle_weak_ptr<U>& other) const
  noexcept
  -> bool {
    return get_control() < other.target_ctrl_;
  }

  template<typename U>
  auto owner_before(const cycle_gptr<U>& other) const
  noexcept
  -> bool {
    return get_control() < other.target_ctrl_;
  }

  template<typename U>
  auto owner_before(const cycle_member_ptr<U>& other) const
  noexcept
  -> bool {
    return get_control() < other.get_control();
  }

 private:
  T* target_ = nullptr;
};

template<typename T>
class cycle_gptr {
  template<typename> friend class cycle_member_ptr;
  template<typename> friend class cycle_gptr;
  template<typename> friend class cycle_weak_ptr;

  template<typename Type, typename Alloc, typename... Args>
  friend auto cycle_ptr::allocate_cycle(Alloc&& alloc, Args&&... args) -> cycle_gptr<Type>;

 public:
  using element_type = std::remove_extent_t<T>;
  using weak_type = cycle_weak_ptr<T>;

  constexpr cycle_gptr() noexcept {}

  constexpr cycle_gptr([[maybe_unused]] std::nullptr_t nil) noexcept
  : cycle_gptr()
  {}

  cycle_gptr(const cycle_gptr& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {
    if (target_ctrl_ != nullptr) target_ctrl_->acquire_no_red();
  }

  cycle_gptr(cycle_gptr&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_gptr(const cycle_gptr<U>& other)
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {
    if (target_ctrl_ != nullptr) target_ctrl_->acquire_no_red();
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_gptr(cycle_gptr<U>&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_gptr(const cycle_member_ptr<U>& other)
  : target_(other.target_),
    target_ctrl_(other.get_control())
  {
    other.throw_if_owner_expired();
    if (target_ctrl_ != nullptr) target_ctrl_->acquire();
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_gptr(cycle_member_ptr<U>&& other)
  : cycle_gptr(other)
  {
    other.reset();
  }

  template<typename U>
  cycle_gptr(const cycle_gptr<U>& other, element_type* target) noexcept
  : target_(target),
    target_ctrl_(other.target_ctrl_)
  {
    if (target_ctrl_ != nullptr) target_ctrl_->acquire_no_red();
  }

  template<typename U>
  cycle_gptr(const cycle_member_ptr<U>& other, element_type* target)
  : target_(target),
    target_ctrl_(other.get_control())
  {
    other.throw_if_owner_expired();
    if (target_ctrl_ != nullptr) target_ctrl_->acquire();
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  explicit cycle_gptr(const cycle_weak_ptr<U>& other)
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {
    if (target_ctrl_ == nullptr || !target_ctrl_->weak_acquire())
      throw std::bad_weak_ptr();
  }

  auto operator=(const cycle_gptr& other)
  noexcept
  -> cycle_gptr& {
    detail::intrusive_ptr<detail::base_control> bc = other.target_ctrl_;
    if (bc != nullptr) bc->acquire_no_red();

    target_ = other.target_;
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  auto operator=(cycle_gptr&& other)
  noexcept
  -> cycle_gptr& {
    auto bc = std::move(other.target_ctrl_);

    target_ = std::exchange(other.target_, nullptr);
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_gptr<U>& other)
  noexcept
  -> cycle_gptr& {
    detail::intrusive_ptr<detail::base_control> bc = other.target_ctrl_;
    if (bc != nullptr) bc->acquire_no_red();

    target_ = other.target_;
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(cycle_gptr<U>&& other)
  noexcept
  -> cycle_gptr& {
    auto bc = std::move(other.target_ctrl_);

    target_ = std::exchange(other.target_, nullptr);
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_member_ptr<U>& other)
  -> cycle_gptr& {
    other.throw_if_owner_expired();

    detail::intrusive_ptr<detail::base_control> bc = other.get_control();
    if (bc != nullptr) bc->acquire();

    target_ = other.target_;
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(cycle_member_ptr<U>&& other)
  -> cycle_gptr& {
    *this = other;
    other.reset();
    return *this;
  }

  ~cycle_gptr() noexcept {
    if (target_ctrl_ != nullptr)
      target_ctrl_->release();
  }

  auto reset()
  noexcept
  -> void {
    if (target_ctrl_ != nullptr) {
      target_ = nullptr;
      target_ctrl_->release();
      target_ctrl_.reset();
    }
  }

  auto swap(cycle_gptr& other)
  noexcept
  -> void {
    std::swap(target_, other.target_);
    target_ctrl_.swap(other.target_ctrl_);
  }

  auto swap(cycle_member_ptr<T>& other)
  -> void {
    other.swap(*this);
  }

  auto get() const
  noexcept
  -> T* {
    return target_;
  }

  template<bool Enable = !std::is_void_v<T>>
  auto operator*() const
  -> std::enable_if_t<Enable, T>& {
    assert(get() != nullptr);
    return *get();
  }

  template<bool Enable = !std::is_void_v<T>>
  auto operator->() const
  -> std::enable_if_t<Enable, T>* {
    assert(get() != nullptr);
    return get();
  }

  explicit operator bool() const noexcept {
    return get() != nullptr;
  }

  template<typename U>
  auto owner_before(const cycle_weak_ptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.target_ctrl_;
  }

  template<typename U>
  auto owner_before(const cycle_gptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.target_ctrl_;
  }

  template<typename U>
  auto owner_before(const cycle_member_ptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.get_control();
  }

 private:
  auto emplace_(T* new_target, detail::intrusive_ptr<detail::base_control> new_target_ctrl)
  noexcept
  -> void {
    assert(new_target_ctrl == nullptr || new_target != nullptr);
    assert(target_ctrl_ == nullptr);

    target_ = new_target;
    target_ctrl_ = std::move(new_target_ctrl);
  }

  T* target_ = nullptr;
  detail::intrusive_ptr<detail::base_control> target_ctrl_ = nullptr;
};


template<typename T>
class cycle_weak_ptr {
  template<typename> friend class cycle_member_ptr;
  template<typename> friend class cycle_gptr;
  template<typename> friend class cycle_weak_ptr;

 public:
  using element_type = std::remove_extent_t<T>;

  constexpr cycle_weak_ptr() noexcept {}

  cycle_weak_ptr(const cycle_weak_ptr& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {}

  cycle_weak_ptr(cycle_weak_ptr&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_weak_ptr(const cycle_weak_ptr<U>& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_weak_ptr(cycle_weak_ptr<U>&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_weak_ptr(const cycle_gptr<U>& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_weak_ptr(const cycle_member_ptr<U>& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.get_control())
  {}

  auto operator=(const cycle_weak_ptr& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.target_ctrl_;
    return *this;
  }

  auto operator=(cycle_weak_ptr&& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = std::exchange(other.target_, nullptr);
    target_ctrl_.reset(other.target_ctrl_.detach(), false);
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_weak_ptr<U>& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.target_ctrl_;
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(cycle_weak_ptr<U>&& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = std::exchange(other.target_, nullptr);
    target_ctrl_.reset(other.target_ctrl_.detach(), false);
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_gptr<U>& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.target_ctrl_;
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_member_ptr<U>& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.get_control();
    return *this;
  }

  auto reset()
  noexcept
  -> void {
    target_ = nullptr;
    target_ctrl_.reset();
  }

  auto swap(cycle_weak_ptr& other)
  noexcept
  -> void {
    std::swap(target_, other.target_);
    target_ctrl_.swap(other.target_ctrl_);
  }

  auto expired() const
  noexcept
  -> bool {
    return target_ctrl_ == nullptr || target_ctrl_->expired();
  }

  auto lock() const
  noexcept
  -> cycle_gptr<T> {
    cycle_gptr<T> result;
    if (target_ctrl_ != nullptr && target_ctrl_->weak_acquire())
      result.emplace_(target_, target_ctrl_);
    return result;
  }

  template<typename U>
  auto owner_before(const cycle_weak_ptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.target_ctrl_;
  }

  template<typename U>
  auto owner_before(const cycle_gptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.target_ctrl_;
  }

  template<typename U>
  auto owner_before(const cycle_member_ptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.get_control();
  }

 private:
  T* target_ = nullptr; // Valid iff target_ctrl_ != nullptr.
  detail::intrusive_ptr<detail::base_control> target_ctrl_ = nullptr;
};


template<typename T, typename U>
inline auto operator==(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

template<typename T>
inline auto operator==(const cycle_member_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !x;
}

template<typename U>
inline auto operator==([[maybe_unused]] std::nullptr_t x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !y;
}

template<typename T, typename U>
inline auto operator!=(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

template<typename T>
inline auto operator!=(const cycle_member_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return bool(x);
}

template<typename U>
inline auto operator!=([[maybe_unused]] std::nullptr_t x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return bool(y);
}

template<typename T, typename U>
inline auto operator<(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

template<typename T>
inline auto operator<(const cycle_member_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return std::less<typename cycle_member_ptr<T>::element_type*>()(x.get(), nullptr);
}

template<typename U>
inline auto operator<([[maybe_unused]] std::nullptr_t x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return std::less<typename cycle_member_ptr<U>::element_type*>()(nullptr, y.get());
}

template<typename T, typename U>
inline auto operator>(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return y < x;
}

template<typename T>
inline auto operator>(const cycle_member_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return nullptr < x;
}

template<typename U>
inline auto operator>([[maybe_unused]] std::nullptr_t x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return y < nullptr;
}

template<typename T, typename U>
inline auto operator<=(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

template<typename T>
inline auto operator<=(const cycle_member_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !(nullptr < x);
}

template<typename U>
inline auto operator<=([[maybe_unused]] std::nullptr_t x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(y < nullptr);
}

template<typename T, typename U>
inline auto operator>=(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

template<typename T>
inline auto operator>=(const cycle_member_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !(x < nullptr);
}

template<typename U>
inline auto operator>=([[maybe_unused]] std::nullptr_t x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(nullptr < y);
}

template<typename T>
inline auto swap(cycle_member_ptr<T>& x, cycle_member_ptr<T>& y)
noexcept
-> void {
  x.swap(y);
}

template<typename T>
inline auto swap(cycle_member_ptr<T>& x, cycle_gptr<T>& y)
noexcept
-> void {
  x.swap(y);
}


template<typename T, typename U>
inline auto operator==(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

template<typename T>
inline auto operator==(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !x;
}

template<typename U>
inline auto operator==([[maybe_unused]] std::nullptr_t x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !y;
}

template<typename T, typename U>
inline auto operator!=(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

template<typename T>
inline auto operator!=(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return bool(x);
}

template<typename U>
inline auto operator!=([[maybe_unused]] std::nullptr_t x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return bool(y);
}

template<typename T, typename U>
inline auto operator<(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

template<typename T>
inline auto operator<(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return std::less<typename cycle_gptr<T>::element_type*>()(x.get(), nullptr);
}

template<typename U>
inline auto operator<([[maybe_unused]] std::nullptr_t x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return std::less<typename cycle_gptr<U>::element_type*>()(nullptr, y.get());
}

template<typename T, typename U>
inline auto operator>(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return y < x;
}

template<typename T>
inline auto operator>(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return nullptr < x;
}

template<typename U>
inline auto operator>([[maybe_unused]] std::nullptr_t x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return y < nullptr;
}

template<typename T, typename U>
inline auto operator<=(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

template<typename T>
inline auto operator<=(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !(nullptr < x);
}

template<typename U>
inline auto operator<=([[maybe_unused]] std::nullptr_t x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(y < nullptr);
}

template<typename T, typename U>
inline auto operator>=(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

template<typename T>
inline auto operator>=(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !(x < nullptr);
}

template<typename U>
inline auto operator>=([[maybe_unused]] std::nullptr_t x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(nullptr < y);
}

template<typename T>
inline auto swap(cycle_gptr<T>& x, cycle_gptr<T>& y)
noexcept
-> void {
  x.swap(y);
}

template<typename T>
inline auto swap(cycle_gptr<T>& x, cycle_member_ptr<T>& y)
noexcept
-> void {
  x.swap(y);
}


template<typename T>
inline auto swap(cycle_weak_ptr<T>& x, cycle_weak_ptr<T>& y)
noexcept
-> void {
  x.swap(y);
}


template<typename T, typename U>
inline auto operator==(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

template<typename T, typename U>
inline auto operator==(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

template<typename T, typename U>
inline auto operator!=(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

template<typename T, typename U>
inline auto operator!=(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

template<typename T, typename U>
inline auto operator<(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

template<typename T, typename U>
inline auto operator<(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

template<typename T, typename U>
inline auto operator>(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return y < x;
}

template<typename T, typename U>
inline auto operator>(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return y < x;
}

template<typename T, typename U>
inline auto operator<=(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

template<typename T, typename U>
inline auto operator<=(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

template<typename T, typename U>
inline auto operator>=(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

template<typename T, typename U>
inline auto operator>=(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}


/**
 * \brief Allocate a new instance of \p T, using the specificied allocator.
 * \relates cycle_base
 * \details
 * Ensures the type \p T is instantiated correctly, with its control block.
 * \tparam T The type of object to instantiate.
 * \param alloc The allocator to use for allocating the control block.
 * \param args The arguments passed to the constructor of type \p T.
 * \returns A cycle_gptr to the new instance of \p T.
 * \throws std::bad_alloc if allocating a generation fails.
 */
template<typename T, typename Alloc, typename... Args>
auto allocate_cycle(Alloc&& alloc, Args&&... args)
-> cycle_gptr<T> {
  using alloc_t = typename std::allocator_traits<Alloc>::template rebind_alloc<T>;
  using control_t = detail::control<T, alloc_t>;
  using alloc_traits = typename std::allocator_traits<Alloc>::template rebind_traits<control_t>;
  using ctrl_alloc_t = typename std::allocator_traits<Alloc>::template rebind_alloc<control_t>;

  ctrl_alloc_t ctrl_alloc = std::forward<Alloc>(alloc);

  control_t* raw_ctrl_ptr = alloc_traits::allocate(ctrl_alloc, 1);
  try {
    alloc_traits::construct(ctrl_alloc, raw_ctrl_ptr, ctrl_alloc);
  } catch (...) {
    alloc_traits::deallocate(ctrl_alloc, raw_ctrl_ptr, 1);
    throw;
  }
  auto ctrl_ptr = detail::intrusive_ptr<detail::base_control>(raw_ctrl_ptr, false);
  T* elem_ptr = raw_ctrl_ptr->instantiate(std::forward<Args>(args)...);

  cycle_gptr<T> result;
  result.emplace_(elem_ptr, std::move(ctrl_ptr));
  return result;
}

/**
 * \brief Allocate a new instance of \p T, using the default allocator.
 * \relates cycle_base
 * \details
 * Ensures the type \p T is instantiated correctly, with its control block.
 *
 * Equivalent to calling ``allocate_cycle<T>(std::allocator<T>(), args...)``.
 * \tparam T The type of object to instantiate.
 * \param args The arguments passed to the constructor of type \p T.
 * \returns A cycle_gptr to the new instance of \p T.
 * \throws std::bad_alloc if allocating a generation fails.
 */
template<typename T, typename... Args>
auto make_cycle(Args&&... args)
-> cycle_gptr<T> {
  return allocate_cycle<T>(std::allocator<T>(), std::forward<Args>(args)...);
}


} /* namespace cycle_ptr */

namespace std {


/**
 * \brief Specialize std::exchange.
 * \relates cycle_ptr::cycle_member_ptr
 * \details
 * Assigns \p y to \p x, returning the previous value of \p x.
 *
 * Specialization is required, because std::exchange creates a copy of \p x,
 * which in this case is not copy-constructible.
 * \returns cycle_gptr with the original value in \p x.
 */
template<typename T, typename U = cycle_ptr::cycle_gptr<T>>
auto exchange(cycle_ptr::cycle_member_ptr<T>& x, U&& y) {
  cycle_ptr::cycle_gptr<T> result = std::move(x);
  x = std::forward<U>(y);
  return result;
}

template<typename T>
struct hash<cycle_ptr::cycle_member_ptr<T>> {
  [[deprecated]]
  typedef cycle_ptr::cycle_member_ptr<T> argument_type;
  [[deprecated]]
  typedef std::size_t result_type;

  auto operator()(const cycle_ptr::cycle_member_ptr<T>& p) const
  noexcept
  -> std::size_t {
    return std::hash<typename cycle_ptr::cycle_member_ptr<T>::element_type*>()(p.get());
  }
};

template<typename T>
struct hash<cycle_ptr::cycle_gptr<T>>
: hash<cycle_ptr::cycle_member_ptr<T>>
{
  [[deprecated]]
  typedef cycle_ptr::cycle_gptr<T> argument_type;
  [[deprecated]]
  typedef std::size_t result_type;

  using hash<cycle_ptr::cycle_member_ptr<T>>::operator();

  auto operator()(const cycle_ptr::cycle_gptr<T>& p) const
  noexcept
  -> std::size_t {
    return std::hash<typename cycle_ptr::cycle_gptr<T>::element_type*>()(p.get());
  }
};


///\brief Perform static cast on pointer.
///\relates cycle_ptr::cycle_gptr
template<typename T, typename U>
auto static_pointer_cast(const cycle_ptr::cycle_gptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      static_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform dynamic cast on pointer.
///\relates cycle_ptr::cycle_gptr
template<typename T, typename U>
auto dynamic_pointer_cast(const cycle_ptr::cycle_gptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      dynamic_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform const cast on pointer.
///\relates cycle_ptr::cycle_gptr
template<typename T, typename U>
auto const_pointer_cast(const cycle_ptr::cycle_gptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      const_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform reinterpret cast on pointer.
///\relates cycle_ptr::cycle_gptr
template<typename T, typename U>
auto reinterpret_pointer_cast(const cycle_ptr::cycle_gptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      reinterpret_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}


///\brief Perform static cast on pointer.
///\relates cycle_ptr::cycle_member_ptr
template<typename T, typename U>
auto static_pointer_cast(const cycle_ptr::cycle_member_ptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      static_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform dynamic cast on pointer.
///\relates cycle_ptr::cycle_member_ptr
template<typename T, typename U>
auto dynamic_pointer_cast(const cycle_ptr::cycle_member_ptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      dynamic_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform const cast on pointer.
///\relates cycle_ptr::cycle_member_ptr
template<typename T, typename U>
auto const_pointer_cast(const cycle_ptr::cycle_member_ptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      const_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform reinterpret cast on pointer.
///\relates cycle_ptr::cycle_member_ptr
template<typename T, typename U>
auto reinterpret_pointer_cast(const cycle_ptr::cycle_member_ptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      reinterpret_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}


} /* namespace std */

#endif /* CYCLE_PTR_CYCLE_PTR_H */
