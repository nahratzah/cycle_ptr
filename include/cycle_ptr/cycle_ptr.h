#ifndef CYCLE_PTR_CYCLE_PTR_H
#define CYCLE_PTR_CYCLE_PTR_H

#include <cycle_ptr/detail/control.h>
#include <cycle_ptr/detail/vertex.h>

namespace cycle_ptr {


template<typename> class cycle_member_ptr;
template<typename> class cycle_gptr;
template<typename> class cycle_weak_ptr;

class cycle_base {
  template<typename> friend class cycle_member_ptr;

 protected:
  cycle_base()
  : control_(detail::base_control::publisher::lookup(this, sizeof(this)))
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
  const boost::intrusive_ptr<detail::base_control> control_;
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

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_member_ptr(cycle_base& owner, const cycle_member_ptr<U>& ptr)
  : vertex(owner),
    target_(ptr.target_)
  {
    ptr.throw_if_owner_expired();
    this->vertex::reset(ptr.get_control(), false, false);
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_member_ptr(cycle_base& owner, cycle_member_ptr<U>&& ptr)
  : cycle_member_ptr(ptr)
  {
    ptr.reset();
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_member_ptr(cycle_base& owner, const cycle_gptr<U>& ptr)
  : vertex(owner),
    target_(ptr.target_)
  {
    this->vertex::reset(ptr.target_ctrl_, false, true);
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_member_ptr(cycle_base& owner, cycle_gptr<U>&& ptr)
  : vertex(owner),
    target_(std::exchange(ptr.target_, nullptr))
  {
    this->vertex::reset(
        boost::intrusive_ptr<base_control>(ptr.target_ctrl_.detach(), false),
        true, true);
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_member_ptr(cycle_base& owner, const cycle_weak_ptr<U>& ptr)
  : cycle_member_ptr(owner, cycle_gptr<U>(ptr))
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
    this->vertex::reset(other.get_control(), false, false);
    return *this;
  }

  auto operator=(cycle_member_ptr&& other)
  -> cycle_member_ptr& {
    *this = other;
    other.reset();
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(const cycle_member_ptr<U>& other)
  -> cycle_member_ptr& {
    other.throw_if_owner_expired();

    target_ = other.target_;
    this->vertex::reset(other.get_control(), false, false);
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(cycle_member_ptr<U>&& other)
  -> cycle_member_ptr& {
    *this = other;
    other.reset();
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(const cycle_gptr<U>& other)
  -> cycle_member_ptr& {
    target_ = other.target_;
    this->vertex::reset(other.target_ctrl_, false, true);
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(cycle_gptr<U>&& other)
  -> cycle_member_ptr& {
    target_ = std::exchange(other.target_, nullptr);
    this->vertex::reset(
        boost::intrusive_ptr<base_control>(other.target_ctrl_.detach(), false),
        false, true);
    return *this;
  }

  auto reset()
  -> void {
    target_ = nullptr;
    this->vertex::reset();
  }

  auto swap(cycle_member_ptr& other)
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

 public:
  using element_type = std::remove_extent_t<T>;
  using weak_type = cycle_weak_ptr<T>;

  constexpr cycle_gptr() noexcept = default;

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

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_gptr(const cycle_gptr<U>& other)
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {
    if (target_ctrl_ != nullptr) target_ctrl_->acquire_no_red();
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_gptr(cycle_gptr<U>&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  explicit cycle_gptr(const cycle_member_ptr<U>& other)
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {
    other.throw_if_owner_expired();
    if (target_ctrl_ != nullptr) target_ctrl_->acquire();
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  explicit cycle_gptr(cycle_member_ptr<U>&& other)
  : cycle_gptr(other)
  {
    other.reset();
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
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
    boost::intrusive_ptr<detail::base_control> bc = other.target_ctrl_;
    if (bc != nullptr) bc->acquire_no_red();

    target_ = other.target_;
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  auto operator=(cycle_gptr&& other)
  noexcept
  -> cycle_gptr& {
    auto bc = boost::intrusive_ptr<detail::base_control>(other.target_ctrl_.detach(), false);

    target_ = std::exchange(other.target_, nullptr);
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(const cycle_gptr<U>& other)
  noexcept
  -> cycle_gptr& {
    boost::intrusive_ptr<detail::base_control> bc = other.target_ctrl_;
    if (bc != nullptr) bc->acquire_no_red();

    target_ = other.target_;
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(cycle_gptr<U>&& other)
  noexcept
  -> cycle_gptr& {
    auto bc = boost::intrusive_ptr<detail::base_control>(other.target_ctrl_.detach(), false);

    target_ = std::exchange(other.target_, nullptr);
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(const cycle_member_ptr<U>& other)
  -> cycle_gptr& {
    other.throw_if_owner_expired();

    boost::intrusive_ptr<detail::base_control> bc = other.get_control();
    if (bc != nullptr) bc->acquire();

    target_ = other.target_;
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
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
  auto emplace_(T* new_target, boost::intrusive_ptr<detail::base_control> new_target_ctrl)
  noexcept
  -> void {
    assert(new_target_ctrl == nullptr || new_target != nullptr);
    assert(target_ctrl_ == nullptr);

    target_ = new_target;
    target_ctrl_.reset(new_target_ctrl.detach(), false);
  }

  T* target_ = nullptr;
  boost::intrusive_ptr<detail::base_control> target_ctrl_ = nullptr;
};


template<typename T>
class cycle_weak_ptr {
  template<typename> friend class cycle_member_ptr;
  template<typename> friend class cycle_gptr;
  template<typename> friend class cycle_weak_ptr;

 public:
  using element_type = std::remove_extent_t<T>;

  constexpr cycle_weak_ptr() noexcept = default;

  cycle_weak_ptr(const cycle_weak_ptr& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {}

  cycle_weak_ptr(cycle_weak_ptr&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_weak_ptr(const cycle_weak_ptr<U>& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_weak_ptr(cycle_weak_ptr<U>&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  cycle_weak_ptr(const cycle_gptr<U>& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {}

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
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

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(const cycle_weak_ptr<U>& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.target_ctrl_;
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(cycle_weak_ptr<U>&& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = std::exchange(other.target_, nullptr);
    target_ctrl_.reset(other.target_ctrl_.detach(), false);
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
  auto operator=(const cycle_gptr<U>& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.target_ctrl_;
    return *this;
  }

  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>
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
  -> cycle_gptr {
    cycle_gptr result;
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
  boost::intrusive_ptr<detail::base_control> target_ctrl_ = nullptr;
};


template<typename T>
inline auto swap(cycle_member_ptr<T>& x, cycle_member_ptr<T>& y)
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

template<typename T>
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

template<typename T>
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

template<typename T, typename U>
inline auto operator<(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return std::less<typename cycle_gptr<T>::element_type*>()(x.get(), nullptr);
}

template<typename T, typename U>
inline auto operator<([[maybe_unused]] std::nullptr_t x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return std::less<typename cycle_gptr<T>::element_type*>()(nullptr, y.get());
}

template<typename T, typename U>
inline auto operator>(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return y < x;
}

template<typename T, typename U>
inline auto operator>(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return nullptr < x;
}

template<typename T, typename U>
inline auto operator>([[maybe_unused]] std::nullptr_t x, const cycle_gptr<T>& y)
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

template<typename T, typename U>
inline auto operator<=(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !(nullptr < x);
}

template<typename T, typename U>
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

template<typename T, typename U>
inline auto operator>=(const cycle_gptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !(x < nullptr);
}

template<typename T, typename U>
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
inline auto swap(cycle_weak_ptr<T>& x, cycle_weak_ptr<T>& y)
noexcept
-> void {
  x.swap(y);
}


} /* namespace cycle_ptr */

namespace std {


/**
 * \brief Specialize std::exchange.
 * \details
 * Assigns \p y to \p x, returning the previous value of \p x.
 *
 * Specialization is required, because std::exchange creates a copy of \p x,
 * which in this case is not copy-constructible.
 * \returns cycle_gptr with the original value in \p x.
 */
template<typename T, typename U = cycle_ptr::cycle_gptr<T>>
auto exchange(cycle_ptr::cycle_member_ptr<T>& x, U&& y) {
  cycle_gptr<T> result = std::move(x);
  x = std::forward<U>(y);
  return result;
}

template<typename T>
struct hash<cycle_ptr::cycle_member_ptr<T>> {
  [[deprecated]]
  using argument_type = cycle_ptr::cycle_member_ptr<T>;
  [[deprecated]]
  using result_type = std::size_t;

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
  using argument_type = cycle_ptr::cycle_gptr<T>;
  [[deprecated]]
  using result_type = std::size_t;

  using hash<cycle_ptr::cycle_member_ptr<T>>::operator();

  auto operator()(const cycle_ptr::cycle_gptr<T>& p) const
  noexcept
  -> std::size_t {
    return std::hash<typename cycle_ptr::cycle_gptr<T>::element_type*>()(p.get());
  }
};


} /* namespace std */

#endif /* CYCLE_PTR_CYCLE_PTR_H */
