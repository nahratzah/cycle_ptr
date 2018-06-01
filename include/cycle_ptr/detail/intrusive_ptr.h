#ifndef CYCLE_PTR_DETAIL_INTRUSIVE_PTR_H
#define CYCLE_PTR_DETAIL_INTRUSIVE_PTR_H

#include <cassert>
#include <cstddef>
#include <functional>
#include <utility>

namespace cycle_ptr::detail {


/**
 * \brief Intrusive pointer.
 * \details
 * Like boost::intrusive_ptr,
 * except with move semantics and constexpr initialization.
 * \tparam T The pointee type.
 */
template<typename T>
class intrusive_ptr {
 public:
  using element_type = T;

  constexpr intrusive_ptr() noexcept = default;

  constexpr intrusive_ptr([[maybe_unused]] std::nullptr_t nil) noexcept
  : intrusive_ptr()
  {}

  intrusive_ptr(T* p, bool acquire) noexcept
  : ptr_(p)
  {
    if (ptr_ != nullptr && acquire)
      intrusive_ptr_add_ref(ptr_); // ADL
  }

  intrusive_ptr(const intrusive_ptr& x) noexcept
  : intrusive_ptr(x.ptr_, true)
  {}

  intrusive_ptr(intrusive_ptr&& x) noexcept
  : ptr_(std::exchange(x.ptr_, nullptr))
  {}

  auto operator=(const intrusive_ptr& x)
  noexcept
  -> intrusive_ptr& {
    T* old = std::exchange(ptr_, x.ptr_);
    if (ptr_ != nullptr)
      intrusive_ptr_add_ref(ptr_); // ADL
    if (old != nullptr)
      intrusive_ptr_release(old); // ADL
    return *this;
  }

  auto operator=(intrusive_ptr&& x)
  noexcept
  -> intrusive_ptr& {
    T* old = std::exchange(ptr_, std::exchange(x.ptr_, nullptr));
    if (old != nullptr)
      intrusive_ptr_release(old); // ADL
    return *this;
  }

  ~intrusive_ptr() noexcept {
    if (ptr_ != nullptr)
      intrusive_ptr_release(ptr_); // ADL
  }

  auto reset() noexcept
  -> void {
    if (ptr_ != nullptr)
      intrusive_ptr_release(std::exchange(ptr_, nullptr)); // ADL
  }

  auto swap(intrusive_ptr& other)
  noexcept
  -> void {
    std::swap(ptr_, other.ptr_);
  }

  auto detach()
  noexcept
  -> T* {
    return std::exchange(ptr_, nullptr);
  }

  auto get() const
  noexcept
  -> T* {
    return ptr_;
  }

  auto operator*() const
  noexcept
  -> T& {
    assert(ptr_ != nullptr);
    return *ptr_;
  }

  auto operator->() const
  noexcept
  -> T* {
    assert(ptr_ != nullptr);
    return ptr_;
  }

  explicit operator bool() const noexcept {
    return ptr_ != nullptr;
  }

 private:
  T* ptr_ = nullptr;
};


///\brief Swap operation.
///\relates intrusive_ptr
template<typename T>
auto swap(intrusive_ptr<T>& x, intrusive_ptr<T>& y)
noexcept
-> void {
  x.swap(y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator==(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator==(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return x == y.get();
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator==(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return x.get() == y;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
auto operator==([[maybe_unused]] std::nullptr_t x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !y;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
auto operator==(const intrusive_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator!=(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator!=(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator!=(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
auto operator!=([[maybe_unused]] std::nullptr_t x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return bool(y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
auto operator!=(const intrusive_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return bool(x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator<(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator<(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return x < y.get();
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator<(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return x.get() < y;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
auto operator<([[maybe_unused]] std::nullptr_t x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return std::less<typename intrusive_ptr<U>::element_type*>()(nullptr, y.get());
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
auto operator<(const intrusive_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return std::less<typename intrusive_ptr<T>::element_type*>()(x.get(), nullptr);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator>(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return y < x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator>(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return y < x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator>(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return y < x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
auto operator>([[maybe_unused]] std::nullptr_t x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return y < nullptr;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
auto operator>(const intrusive_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return nullptr < x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator<=(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator<=(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator<=(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
auto operator<=([[maybe_unused]] std::nullptr_t x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(y < nullptr);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
auto operator<=(const intrusive_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !(nullptr < x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator>=(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator>=(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
auto operator>=(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return !(x < y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
auto operator>=([[maybe_unused]] std::nullptr_t x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(nullptr < y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
auto operator>=(const intrusive_ptr<T>& x, [[maybe_unused]] std::nullptr_t y)
noexcept
-> bool {
  return !(x < nullptr);
}


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_INTRUSIVE_PTR_H */
