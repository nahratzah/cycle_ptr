#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <tuple>
#include <type_traits>
#include <utility>

namespace cycle_ptr {
template<typename> class cycle_allocator;
class gc_operation;


/**
 * \brief Function for delayed GC invocations.
 * \relates gc_operation
 * \details
 * This function is passed a \ref gc_operation "function object" representing
 * a GC invocation.
 * It should ensure this function object will be executed at least once, at
 * some point.
 *
 * Multiple GC requests may occur, for multiple generations.
 * However, a single generation will not issue a secondary request until
 * the GC operation has started.
 *
 * Calling \ref get_delay_gc() or \ref set_delay_gc() during this function
 * will result in dead lock.
 *
 * If the delay_gc function throws an exception, the code will swallow that
 * exception and execute the GC operation immediately.
 * If the function returns normally, it has responsibility for executing the
 * gc_operation argument.
 * \sa \ref set_delay_gc
 * \sa \ref get_delay_gc
 */
using delay_gc = std::function<void(gc_operation)>;

/**
 * \brief Read the current delay_gc function.
 * \relates gc_operation
 * \details
 * Returns the currently installed delay_gc function.
 * Any GC requests will be submitted to the installed function.
 *
 * If the returned function is empty (i.e. ``nullptr``),
 * GC operations are not delayed.
 * \returns The currently installed delay_gc function.
 */
auto get_delay_gc() -> delay_gc;

/**
 * \brief Set the delay_gc function.
 * \relates gc_operation
 * \details
 * This method synchronizes with all its invocations,
 * ensuring that no calls to the old function are in progress
 * when this function returns.
 *
 * This could for example be used in combination with ``boost::asio``,
 * to submit GC operations:
 * \code
 * boost::asio::io_context io_context;
 *
 * set_delay_gc(
 *     [&io_contex](gc_operation op) {
 *       boost::asio::post(io_context, std::move(op));
 *     });
 * \endcode
 * \param[in] f \parblock
 * The function to install for delayed GC.
 * Future GC requests will post using the installed function \p f.
 *
 * If the function is empty (i.e. ``nullptr``), future GC operations
 * will be executed immediately.
 * \endparblock
 * \returns The previous delay_gc function
 */
auto set_delay_gc(delay_gc f) -> delay_gc;


} /* namespace cycle_ptr */


namespace cycle_ptr::detail {


class base_control;
class generation;


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

  constexpr intrusive_ptr(std::nullptr_t nil [[maybe_unused]]) noexcept
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
inline auto swap(intrusive_ptr<T>& x, intrusive_ptr<T>& y)
noexcept
-> void {
  x.swap(y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator==(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator==(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return x == y.get();
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator==(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return x.get() == y;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
inline auto operator==(std::nullptr_t x [[maybe_unused]], const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !y;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
inline auto operator==(const intrusive_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return !x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator!=(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator!=(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator!=(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
inline auto operator!=(std::nullptr_t x [[maybe_unused]], const intrusive_ptr<U>& y)
noexcept
-> bool {
  return bool(y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
inline auto operator!=(const intrusive_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return bool(x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator<(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator<(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return x < y.get();
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator<(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return x.get() < y;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
inline auto operator<(std::nullptr_t x [[maybe_unused]], const intrusive_ptr<U>& y)
noexcept
-> bool {
  return std::less<typename intrusive_ptr<U>::element_type*>()(nullptr, y.get());
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
inline auto operator<(const intrusive_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return std::less<typename intrusive_ptr<T>::element_type*>()(x.get(), nullptr);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator>(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return y < x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator>(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return y < x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator>(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return y < x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
inline auto operator>(std::nullptr_t x [[maybe_unused]], const intrusive_ptr<U>& y)
noexcept
-> bool {
  return y < nullptr;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
inline auto operator>(const intrusive_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return nullptr < x;
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator<=(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator<=(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator<=(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
inline auto operator<=(std::nullptr_t x [[maybe_unused]], const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(y < nullptr);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
inline auto operator<=(const intrusive_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return !(nullptr < x);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator>=(const intrusive_ptr<T>& x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator>=(T* x, const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T, typename U>
inline auto operator>=(const intrusive_ptr<T>& x, U* y)
noexcept
-> bool {
  return !(x < y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename U>
inline auto operator>=(std::nullptr_t x [[maybe_unused]], const intrusive_ptr<U>& y)
noexcept
-> bool {
  return !(nullptr < y);
}

///\brief Comparison.
///\relates intrusive_ptr
template<typename T>
inline auto operator>=(const intrusive_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return !(x < nullptr);
}

///\brief Write pointer to output stream.
///\relates intrusive_ptr
template<typename Char, typename Traits, typename T>
inline auto operator<<(std::basic_ostream<Char, Traits>& out, const intrusive_ptr<T>& ptr)
-> std::basic_ostream<Char, Traits>& {
  return out << ptr.get();
}


///\brief Tag used to indicate a link is to be the root of a linked list.
struct llist_head_tag {};

template<typename> class link;


/**
 * \brief Intrusive linked list.
 * \tparam T The element type of the linked list.
 * \tparam Tag A tag used to identify the \ref link of the linked list.
 */
template<typename T, typename Tag>
class llist
: private link<Tag>
{
  static_assert(std::is_base_of_v<link<Tag>, T>,
      "Require T to derive from link<Tag>.");

 public:
  ///\brief Value type of the list.
  using value_type = T;
  ///\brief Reference type of the list.
  using reference = T&;
  ///\brief Const reference type of the list.
  using const_reference = const T&;
  ///\brief Pointer type of the list.
  using pointer = T*;
  ///\brief Const pointer type of the list.
  using const_pointer = const T*;
  ///\brief Size type of the list.
  using size_type = std::uintptr_t;

  class iterator;
  class const_iterator;

  ///\brief Reverse iterator type.
  using reverse_iterator = std::reverse_iterator<iterator>;
  ///\brief Reverse const_iterator type.
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  ///\brief Default constructor.
  llist() noexcept
  : link<Tag>(llist_head_tag())
  {}

  llist(const llist&) = delete;
  auto operator=(const llist&) -> llist& = delete;

  ///\brief Move constructor.
  llist(llist&& other) noexcept
  : link<Tag>()
  {
    splice(end(), other);
  }

  ///\brief Range constructor.
  ///\details Elements in supplied range will be linked by reference.
  ///\param b,e Iterator pair describing a range of elements to insert to the list.
  template<typename Iter>
  llist(Iter b, Iter e)
  : llist()
  {
    insert(end(), b, e);
  }

  ///\brief Destructor.
  ///\details Unlinks all elements.
  ~llist() noexcept {
    clear();
  }

  ///\brief Test if this list is empty.
  ///\returns True if the list holds no elements, false otherwise.
  auto empty() const
  noexcept
  -> bool {
    return this->pred_ == this;
  }

  ///\brief Compute the size of the list.
  ///\returns Number of elements in the list.
  ///\note Linear complexity, as the list does not maintain an internal size.
  auto size() const
  noexcept
  -> size_type {
    return static_cast<size_type>(std::distance(begin(), end()));
  }

  ///\brief Unlink all elements from the list.
  auto clear()
  noexcept
  -> void {
    erase(begin(), end());
  }

  ///\brief Create iterator to element.
  ///\details Undefined behaviour if the element is not linked.
  ///\param elem An element that is on the list.
  static auto iterator_to(T& elem)
  noexcept
  -> iterator {
    return iterator(std::addressof(elem));
  }

  ///\brief Create iterator to element.
  ///\details Undefined behaviour if the element is not linked.
  ///\param elem An element that is on the list.
  static auto iterator_to(const T& elem)
  noexcept
  -> const_iterator {
    return const_iterator(std::addressof(elem));
  }

  ///\brief Reference to first item in this list.
  ///\details Undefined behaviour if this list is empty.
  ///\returns Reference to first element in the list.
  ///\pre !this->empty()
  auto front()
  -> T& {
    assert(!empty());
    return *begin();
  }

  ///\brief Reference to first item in this list.
  ///\details Undefined behaviour if this list is empty.
  ///\returns Reference to first element in the list.
  ///\pre !this->empty()
  auto front() const
  -> const T& {
    assert(!empty());
    return *begin();
  }

  ///\brief Reference to last item in this list.
  ///\details Undefined behaviour if this list is empty.
  ///\returns Reference to last element in the list.
  ///\pre !this->empty()
  auto back()
  -> T& {
    assert(!empty());
    return *std::prev(end());
  }

  ///\brief Reference to last item in this list.
  ///\details Undefined behaviour if this list is empty.
  ///\returns Reference to last element in the list.
  ///\pre !this->empty()
  auto back() const
  -> const T& {
    assert(!empty());
    return *std::prev(end());
  }

  ///\brief Return iterator to first element in the list.
  auto begin()
  noexcept
  -> iterator {
    return iterator(this->succ_);
  }

  ///\brief Return iterator to first element in the list.
  auto begin() const
  noexcept
  -> const_iterator {
    return cbegin();
  }

  ///\brief Return iterator to first element in the list.
  auto cbegin() const
  noexcept
  -> const_iterator {
    return const_iterator(this->succ_);
  }

  ///\brief Return iterator past the last element in the list.
  auto end()
  noexcept
  -> iterator {
    return iterator(this);
  }

  ///\brief Return iterator past the last element in the list.
  auto end() const
  noexcept
  -> const_iterator {
    return cend();
  }

  ///\brief Return iterator past the last element in the list.
  auto cend() const
  noexcept
  -> const_iterator {
    return const_iterator(this);
  }

  ///\brief Return iterator to first element in reverse iteration.
  auto rbegin()
  noexcept
  -> reverse_iterator {
    return reverse_iterator(end());
  }

  ///\brief Return iterator to first element in reverse iteration.
  auto rbegin() const
  noexcept
  -> const_reverse_iterator {
    return crbegin();
  }

  ///\brief Return iterator to first element in reverse iteration.
  auto crbegin() const
  noexcept
  -> const_reverse_iterator {
    return const_reverse_iterator(cend());
  }

  ///\brief Return iterator past the last element in reverse iteration.
  auto rend()
  noexcept
  -> reverse_iterator {
    return reverse_iterator(begin());
  }

  ///\brief Return iterator past the last element in reverse iteration.
  auto rend() const
  noexcept
  -> const_reverse_iterator {
    return crend();
  }

  ///\brief Return iterator past the last element in reverse iteration.
  auto crend() const
  noexcept
  -> const_reverse_iterator {
    return const_reverse_iterator(cbegin());
  }

  ///\brief Link element into the list, as the last item.
  ///\post &this->last() == &v
  auto push_back(T& v)
  noexcept
  -> void {
    insert(end(), v);
  }

  ///\brief Link element into the list, as the first item.
  ///\post &this->front() == &v
  auto push_front(T& v)
  noexcept
  -> void {
    insert(begin(), v);
  }

  ///\brief Link element into the list.
  ///\param pos Iterator to position in front of which the element will be inserted.
  ///\param v The element to insert.
  ///\returns Iterator to \p v.
  auto insert(const_iterator pos, T& v) noexcept -> iterator;

  ///\brief Link multiple elements into the list.
  ///\param pos Iterator to position in front of which the element will be inserted.
  ///\param b,e Range of elements to link into the list.
  ///\returns Iterator to first element in range [\p b, \p e).
  ///Or \p pos if the range is empty.
  template<typename Iter>
  auto insert(const_iterator pos, Iter b, Iter e) -> iterator;

  ///\brief Unlink the first element in the list.
  ///\details Undefined behaviour if this list is empty.
  ///\returns Reference to unlinked element.
  ///\pre !this->empty()
  auto pop_front()
  -> T& {
    assert(!empty());
    T& result = front();
    erase(begin());
    return result;
  }

  ///\brief Unlink the last element in the list.
  ///\details Undefined behaviour if this list is empty.
  ///\returns Reference to unlinked element.
  ///\pre !this->empty()
  auto pop_back()
  -> T& {
    assert(!empty());
    T& result = back();
    erase(std::prev(end()));
    return result;
  }

  ///\brief Erase element from the list.
  ///\returns Iterator to the element after \p b.
  ///\pre b is a dereferenceable iterator for this list.
  auto erase(const_iterator b) -> iterator;
  ///\brief Erase elements from the list.
  ///\param b,e Range of elements to erase.
  ///\returns Iterator to the element after \p b (same as \p e).
  ///\pre range [b,e) is dereferencable for this list.
  auto erase(const_iterator b, const_iterator e) -> iterator;

  ///\brief Splice elements from list.
  ///\details All elements in \p other are moved into this list, before \p pos.
  ///\param pos Position before which the elements are inserted.
  ///\param other The list from which to move elements.
  ///\pre this != &other
  auto splice(const_iterator pos, llist& other) noexcept -> void;
  ///\brief Splice elements from list.
  ///\details \p elem in \p other is moved into this list, before \p pos.
  ///\param pos Position before which the elements are inserted.
  ///\param other The list from which to move elements.
  ///\param elem Iterator to the element that is to be moved.
  ///\pre \p elem is a dereferenceable iterator in \p other.
  auto splice(const_iterator pos, llist& other, const_iterator elem) noexcept -> void;
  ///\brief Splice elements from list.
  ///\details \p elem in \p other is moved into this list, before \p pos.
  ///\param pos Position before which the elements are inserted.
  ///\param other The list from which to move elements.
  ///\param other_begin,other_end Range of elements to move into this list.
  ///\pre Range [\p other_begin, \p other_end) is a dereferenceable range in \p other.
  auto splice(const_iterator pos, llist& other, const_iterator other_begin, const_iterator other_end) noexcept -> void;
};


/**
 * \brief Internally used datastructure for \ref llist.
 * \details
 * The link holds the preceding and successive pointers, to enable implementing
 * a doubly linked list.
 *
 * The head constructor (selected using \ref llist_head_tag) turns
 * the linked list into a circular list.
 * \param Tag Discriminant tag.
 */
template<typename Tag>
class link {
  template<typename, typename> friend class llist;

 public:
  ///\brief Default constructor.
  constexpr link() noexcept = default;

  ///\brief Destructor.
#ifndef NDEBUG
  ~link() noexcept {
    assert((pred_ == nullptr && succ_ == nullptr)
        || (pred_ == this && succ_ == this));
  }
#else
  ~link() noexcept = default;
#endif

 protected:
  /**
   * \brief Constructor.
   * \details Link pointers are not a property of derived class,
   * so we don't update the owner list.
   *
   * This constructor exists so that derived classes can have a copy/move
   * constructor defaulted.
   */
  constexpr link(const link& other [[maybe_unused]]) noexcept
  : link()
  {}

  /**
   * \brief Assignment.
   * \details Link pointers are not a property of derived class,
   * so we don't update the owner list.
   *
   * This method exists so that derived classes can have a copy/move
   * assignment defaulted.
   */
  constexpr auto operator=(const link& other [[maybe_unused]]) noexcept
  -> link& {
    return *this;
  }

 private:
  ///\brief Constructor used internally by llist.
  ///\details Constructs link pointing at itself, turning it into the
  ///head of an empty linked list.
  ///\param lht Tag selecting this constructor.
  link(const llist_head_tag& lht [[maybe_unused]]) noexcept
  : pred_(this),
    succ_(this)
  {}

 protected:
  ///\brief Test if this is linked into a linked list.
  ///\returns True if this is on a linked list.
  constexpr auto linked() const
  noexcept
  -> bool {
    return pred_ != nullptr;
  }

 private:
  ///\brief Pointer to preceding link item in the list.
  link* pred_ = nullptr;
  ///\brief Pointer to successive link item in the list.
  link* succ_ = nullptr;
};


/**
 * \brief Iterator for llist.
 */
template<typename T, typename Tag>
class llist<T, Tag>::iterator {
  template<typename, typename> friend class llist;
  friend class llist::const_iterator;

 public:
  ///\copydoc llist::value_type
  using value_type = T;
  ///\copydoc llist::reference
  using reference = T&;
  ///\copydoc llist::pointer
  using pointer = T*;
  ///\brief Difference type of the iterator.
  using difference_type = std::intptr_t;
  ///\brief Iterator is a bidirectional iterator.
  using iterator_category = std::bidirectional_iterator_tag;

 private:
  ///\brief Initializing constructor.
  ///\details Constructs an iterator to \p ptr.
  ///\note If \p ptr is not linked, the iterator is dereferenceable, but not advancable.
  explicit constexpr iterator(link<Tag>* ptr) noexcept
  : link_(ptr)
  {
    assert(ptr != nullptr);
  }

 public:
  ///\brief Default constructor.
  ///\details Creates an iterator that cannot be dereferenced or advanced.
  constexpr iterator() noexcept = default;

  ///\brief Dereference operation.
  ///\pre This iterator is dereferenceable.
  auto operator*() const
  noexcept
  -> T& {
    assert(link_ != nullptr);
    return *static_cast<T*>(link_);
  }

  ///\brief Indirection operation.
  ///\pre This iterator is dereferenceable.
  auto operator->() const
  noexcept
  -> T* {
    assert(link_ != nullptr);
    return static_cast<T*>(link_);
  }

  ///\brief Advance iterator.
  ///\returns *this
  auto operator++()
  noexcept
  -> iterator& {
    assert(link_ != nullptr);
    link_ = link_->succ_;
    return *this;
  }

  ///\brief Move iterator position backward.
  ///\returns *this
  auto operator--()
  noexcept
  -> iterator& {
    assert(link_ != nullptr);
    link_ = link_->pred_;
    return *this;
  }

  ///\brief Advance iterator.
  ///\returns original value of *this
  auto operator++(int)
  noexcept
  -> iterator {
    iterator result = *this;
    ++*this;
    return result;
  }

  ///\brief Move iterator position backward.
  ///\returns original value of *this
  auto operator--(int)
  noexcept
  -> iterator {
    iterator result = *this;
    --*this;
    return result;
  }

  ///\brief Equality comparator.
  auto operator==(const iterator& other) const
  noexcept
  -> bool {
    return link_ == other.link_;
  }

  ///\brief Inequality comparator.
  auto operator!=(const iterator& other) const
  noexcept
  -> bool {
    return !(*this == other);
  }

  ///\brief Equality comparator.
  auto operator==(const const_iterator& other) const
  noexcept
  -> bool {
    return link_ == other.link_;
  }

  ///\brief Inequality comparator.
  auto operator!=(const const_iterator& other) const
  noexcept
  -> bool {
    return !(*this == other);
  }

 private:
  ///\brief Pointer to element.
  link<Tag>* link_ = nullptr;
};

/**
 * \brief Const iterator for llist.
 */
template<typename T, typename Tag>
class llist<T, Tag>::const_iterator {
  template<typename, typename> friend class llist;
  friend class llist::iterator;

 public:
  ///\copydoc llist::iterator::value_type
  using value_type = T;
  ///\copydoc llist::iterator::reference
  using reference = const T&;
  ///\copydoc llist::iterator::pointer
  using pointer = const T*;
  ///\copydoc llist::iterator::difference_type
  using difference_type = std::intptr_t;
  ///\copydoc llist::iterator::iterator_category
  using iterator_category = std::bidirectional_iterator_tag;

 private:
  ///\brief Initializing constructor.
  ///\details Constructs an iterator to \p ptr.
  ///\note If \p ptr is not linked, the iterator is dereferenceable, but not advancable.
  explicit constexpr const_iterator(const link<Tag>* ptr) noexcept
  : link_(ptr)
  {
    assert(ptr != nullptr);
  }

 public:
  ///\brief Default constructor.
  ///\details Creates an iterator that cannot be dereferenced or advanced.
  constexpr const_iterator() noexcept = default;

  ///\brief Conversion constructor.
  ///\details Creates const_iterator from non-const iterator.
  ///\param other Iterator to assign to *this.
  constexpr const_iterator(const iterator& other) noexcept
  : link_(other.link_)
  {}

  ///\brief Conversion assignment.
  ///\details Assigns *this from non-const iterator.
  ///\param other Iterator to assign to *this.
  constexpr auto operator=(const iterator& other)
  noexcept
  -> const_iterator& {
    link_ = other.link_;
    return *this;
  }

  ///\brief Dereference operation.
  ///\pre This iterator is dereferenceable.
  auto operator*() const
  noexcept
  -> const T& {
    assert(link_ != nullptr);
    return *static_cast<const T*>(link_);
  }

  ///\brief Indirection operation.
  ///\pre This iterator is dereferenceable.
  auto operator->() const
  noexcept
  -> const T* {
    assert(link_ != nullptr);
    return static_cast<const T*>(link_);
  }

  ///\brief Advance iterator.
  ///\returns *this
  auto operator++()
  noexcept
  -> const_iterator& {
    assert(link_ != nullptr);
    link_ = link_->succ_;
    return *this;
  }

  ///\brief Move iterator position backward.
  ///\returns *this
  auto operator--()
  noexcept
  -> const_iterator& {
    assert(link_ != nullptr);
    link_ = link_->pred_;
    return *this;
  }

  ///\brief Advance iterator.
  ///\returns original value of *this
  auto operator++(int)
  noexcept
  -> const_iterator {
    const_iterator result = *this;
    ++*this;
    return result;
  }

  ///\brief Move iterator position backward.
  ///\returns original value of *this
  auto operator--(int)
  noexcept
  -> const_iterator {
    const_iterator result = *this;
    --*this;
    return result;
  }

  ///\copydoc llist::iterator::operator==
  auto operator==(const const_iterator& other) const
  noexcept
  -> bool {
    return link_ == other.link_;
  }

  ///\copydoc llist::iterator::operator!=
  auto operator!=(const const_iterator& other) const
  noexcept
  -> bool {
    return !(*this == other);
  }

  ///\copydoc llist::iterator::operator==
  auto operator==(const iterator& other) const
  noexcept
  -> bool {
    return link_ == other.link_;
  }

  ///\copydoc llist::iterator::operator!=
  auto operator!=(const iterator& other) const
  noexcept
  -> bool {
    return !(*this == other);
  }

 private:
  ///\brief Pointer to element.
  const link<Tag>* link_ = nullptr;
};


template<typename T, typename Tag>
inline auto llist<T, Tag>::insert(const_iterator pos, T& v)
noexcept
-> iterator {
  assert(pos.link_ != nullptr);

  link<Tag>*const vlink = std::addressof(v);
  assert(vlink->succ_ == nullptr && vlink->pred_ == nullptr);
  link<Tag>*const succ = const_cast<link<Tag>*>(pos.link_);
  link<Tag>*const pred = succ->pred_;

  vlink->pred_ = std::exchange(succ->pred_, vlink);
  vlink->succ_ = std::exchange(pred->succ_, vlink);
  return iterator(vlink);
}

template<typename T, typename Tag>
template<typename Iter>
inline auto llist<T, Tag>::insert(const_iterator pos, Iter b, Iter e)
-> iterator {
  assert(pos.link_ != nullptr);
  if (b == e) return iterator(const_cast<link<Tag>*>(pos.link_));

  iterator result = insert(pos, *b);
  while (++b != e) insert(pos, *b);
  return result;
}

template<typename T, typename Tag>
inline auto llist<T, Tag>::erase(const_iterator b)
-> iterator {
  assert(b != end());
  return erase(b, std::next(b));
}

template<typename T, typename Tag>
inline auto llist<T, Tag>::erase(const_iterator b, const_iterator e)
-> iterator {
  assert(b.link_ != nullptr && e.link_ != nullptr);

  if (b == e) return iterator(const_cast<link<Tag>*>(e.link_));

  link<Tag>*const pred = b.link_->pred_;
  assert(pred->succ_ == b.link_);
  link<Tag>*const succ = const_cast<link<Tag>*>(e.link_);

  pred->succ_ = succ;
  succ->pred_ = pred;

  while (b != e) {
    link<Tag>*const l = const_cast<link<Tag>*>(b.link_);
    ++b;
    assert(l != this);
    l->pred_ = l->succ_ = nullptr;
  }
  return iterator(succ);
}

template<typename T, typename Tag>
inline auto llist<T, Tag>::splice(const_iterator pos, llist& other)
noexcept
-> void {
  assert(&other != this);
  splice(pos, other, other.begin(), other.end());
}

template<typename T, typename Tag>
inline auto llist<T, Tag>::splice(const_iterator pos, llist& other, const_iterator elem)
noexcept
-> void {
  assert(pos.link_ != nullptr && elem.link_ != nullptr);
  if (elem.link_ == pos.link_) return; // Insert before self.
  splice(pos, other, elem, std::next(elem));
}

template<typename T, typename Tag>
inline auto llist<T, Tag>::splice(const_iterator pos, llist& other, const_iterator other_begin, const_iterator other_end)
noexcept
-> void {
#ifndef NDEBUG
  assert(pos.link_ != nullptr && other_begin.link_ != nullptr && other_end.link_ != nullptr);
  for (const_iterator i = other_begin; i != other_end; ++i)
    assert(pos != i); // Cannot splice inside of range.
  for (const_iterator i = other_begin; i != other_end; ++i)
    assert(other.end() != i); // Cannot splice list head.
#endif

  if (other_begin == other_end) return; // Empty range.
  if (pos == other_end) {
    assert(this == &other);
    return; // Splice into same position.
  }

  link<Tag>*const my_succ = const_cast<link<Tag>*>(pos.link_);
  link<Tag>*const my_pred = my_succ->pred_;
  link<Tag>*const other_first = const_cast<link<Tag>*>(other_begin.link_);
  link<Tag>*const other_pred = other_first->pred_;
  link<Tag>*const other_succ = const_cast<link<Tag>*>(other_end.link_);
  link<Tag>*const other_last = other_succ->pred_;

  my_succ->pred_ = other_last;
  my_pred->succ_ = other_first;

  other_last->succ_ = my_succ;
  other_first->pred_ = my_pred;

  other_succ->pred_ = other_pred;
  other_pred->succ_ = other_succ;
}


#ifdef __cpp_lib_hardware_interference_size
  using std::hardware_destructive_interference_size;
#else
  constexpr std::size_t hardware_destructive_interference_size = 64;
#endif


/**
 * \brief Hazard pointer algorithm.
 * \details
 * While hazard algorithm is in theory lock-free and wait-free,
 * since we cannot create an infinite amount of hazard global storage,
 * we do have some spinning operations, that may strike when thread
 * contention is high.
 *
 * \tparam T The element type of the pointer.
 */
template<typename T>
class hazard {
 private:
  /**
   * \brief Cache line aligned pointer.
   * \details
   * We align the atomic pointers, to prevent false sharing.
   *
   * Padding is added to make the data type not only cache-line aligned,
   * but also cache-line sized.
   */
  struct alignas(hardware_destructive_interference_size) data {
    static_assert(sizeof(std::atomic<T*>) < hardware_destructive_interference_size,
        "Cycle_ptr did not expect a platform where cache line is less than or equal to a pointer.");

    std::atomic<T*> ptr = nullptr;
    [[maybe_unused]] char pad_[hardware_destructive_interference_size - sizeof(std::atomic<T*>)];
  };

  /**
   * \brief List of hazard pointers.
   * \details
   * We make this the size of the most common page size.
   *
   * Coupled with alignment, this will ensure a single TLB entry
   * can cover the entire hazard range.
   */
  using ptr_set = std::array<data, 4096u / sizeof(data)>;

 public:
  ///\brief Pointer used by this algorithm.
  using pointer = intrusive_ptr<T>;

  hazard(const hazard&) = delete;

  ///\brief Create hazard context.
  ///\details Used for reading hazard pointers.
  explicit hazard() noexcept
  : d_(allocate_())
  {}

  ///\brief Load value in ptr.
  ///\returns The value of ptr. Returned value has ownership.
  [[nodiscard]]
  auto operator()(const std::atomic<T*>& ptr)
  noexcept
  -> pointer {
    return (*this)(ptr, ptr.load(std::memory_order_relaxed));
  }

 private:
  ///\brief Load value in ptr.
  ///\returns The value of ptr. Returned value has ownership.
  [[nodiscard]]
  auto operator()(const std::atomic<T*>& ptr, T* target)
  noexcept
  -> pointer {
    for (;;) {
      // Nullptr case is trivial.
      if (target == nullptr) return nullptr;

      // Publish intent to acquire 'target'.
      for (;;) {
        T* expect = nullptr;
        if (d_.ptr.compare_exchange_strong(
                expect,
                target,
                std::memory_order_acquire,
                std::memory_order_relaxed)) [[likely]] {
          break;
        }
      }

      // Check that ptr (still or again) holds 'target'.
      {
        T*const tmp = ptr.load(std::memory_order_acquire);
        if (tmp != target) [[unlikely]] {
          // Clear published value.
          T* expect = target;
          if (!d_.ptr.compare_exchange_strong(
                  expect,
                  nullptr,
                  std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
            // ABA problem:
            // Since we don't know if the granted reference is the
            // pointer that was originally assigned to ptr, or a newly
            // allocated value at the same address, we
            // have no option but to discard it.
            if (ptr.load(std::memory_order_relaxed) == target) [[unlikely]] {
              return pointer(target, false);
            }

            // Another thread granted us a reference.
            release_(target);
          }

          target = tmp; // Update target to newly found value.
          continue; // Restart after cancellation.
        }
      }

      // Current state:
      // 1. intent published
      // 2. intent is valid
      // Thus, 'target' will have life time guarantee.
      acquire_(target);

      T* expect = target;
      if (!d_.ptr.compare_exchange_strong(
              expect,
              nullptr,
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        // Another thread granted us a reference, meaning we have 2.
        // We want only 1.
        release_(target);
      }
      return pointer(target, false); // NOLINT (clang-tidy doesn't read the above comment about 2 references, and thinks this is a use-after-free.)
    }
  }

 public:
  /**
   * \brief Release pointer, granting it to a hazard operation, if possible.
   * \details
   * Releases the pointer in a way that another hazard may be able to take
   * ownership of \p ptr.
   *
   * Not needed in normal operation.
   * Only required during atomic ptr resets.
   *
   * Because we only require this for pointers participating in the hazards
   * (i.e., atomic pointers), we have to fill in all hazards at release.
   *
   * If we wanted to assign to only one, we would require all pointer resets
   * to go through the hazards.
   * Which would be potentially error prone, not to mention cause a lot of
   * overhead which could be entirely avoided in unshared cases.
   */
  static auto release(T*&& ptr)
  noexcept
  -> void {
    if (ptr == nullptr) return; // Nullptr case is trivial.

    bool two_refs = false;
    for (data& d : ptr_set_()) {
      if (!std::exchange(two_refs, true))
        acquire_(ptr);

      T* expect = ptr;
      if (d.ptr.compare_exchange_strong(
              expect,
              nullptr,
              std::memory_order_release,
              std::memory_order_relaxed)) {
        // Granted one reference to active hazard.
        two_refs = false;
      }
    }

    if (std::exchange(two_refs, false)) release_(ptr);
    release_(ptr);
    ptr = nullptr;
  }

  /**
   * \brief Reset the pointer.
   * \details
   * Assigns a nullptr value to \p ptr.
   *
   * Does the correct thing to ensure the life time invariant of hazard
   * is maintained.
   */
  static auto reset(std::atomic<T*>& ptr)
  noexcept
  -> void {
    release(ptr.exchange(nullptr, std::memory_order_release));
  }

  /**
   * \brief Reset the pointer to the given new value.
   * \details
   * Assigns \p new_value to \p ptr.
   *
   * Does the correct thing to ensure the life time invariant of hazard
   * is maintained.
   *
   * \param[in,out] ptr The atomic pointer that is to be assigned to.
   * \param[in] new_value The newly assigned pointer value.
   *  Ownership is transferred to \p ptr.
   */
  static auto reset(std::atomic<T*>& ptr, pointer&& new_value)
  noexcept
  -> void {
    release(ptr.exchange(new_value.detach(), std::memory_order_release));
  }

  /**
   * \brief Reset the pointer to the given new value.
   * \details
   * Assigns \p new_value to \p ptr.
   *
   * Does the correct thing to ensure the life time invariant of hazard
   * is maintained.
   *
   * \param[in,out] ptr The atomic pointer that is to be assigned to.
   * \param[in] new_value The newly assigned pointer value.
   */
  static auto reset(std::atomic<T*>& ptr, const pointer& new_value)
  noexcept
  -> void {
    reset(ptr, pointer(new_value));
  }

  /**
   * \brief Exchange the pointer.
   * \details
   * Clears the store pointer and returns the previous value.
   */
  static auto exchange(std::atomic<T*>& ptr, [[maybe_unused]] std::nullptr_t new_value)
  noexcept
  -> pointer {
    T*const rv = ptr.exchange(nullptr, std::memory_order_acq_rel);
    release(acquire_(rv)); // Must grant old value to active hazards.
    return pointer(rv, false);
  }

  /**
   * \brief Exchange the pointer.
   * \details
   * Stores the pointer \p new_value in the hazard and returns the previous value.
   */
  static auto exchange(std::atomic<T*>& ptr, pointer&& new_value)
  noexcept
  -> pointer {
    T*const rv = ptr.exchange(new_value.detach(), std::memory_order_acq_rel);
    release(acquire_(rv)); // Must grant old value to active hazards.
    return pointer(rv, false);
  }

  /**
   * \brief Exchange the pointer.
   * \details
   * Stores the pointer \p new_value in the hazard and returns the previous value.
   */
  static auto exchange(std::atomic<T*>& ptr, const pointer& new_value)
  noexcept
  -> pointer {
    return exchange(ptr, pointer(new_value));
  }

  /**
   * \brief Compare-exchange operation.
   * \details
   * Replaces \p ptr with \p desired, if it is equal to \p expected.
   *
   * If this fails, \p expected is updated with the value stored in \p ptr.
   *
   * This weak operation may fail despite \p ptr holding \p expected.
   * \param ptr The atomic pointer to change.
   * \param expected The expected value of \p ptr.
   * \param desired The value to assign to \p ptr, if \p ptr holds \p expected.
   */
  static auto compare_exchange_weak(std::atomic<T*>& ptr, pointer& expected, pointer desired)
  noexcept
  -> bool {
    T* expect = expected.get();
    if (ptr.compare_exchange_weak(
            expect,
            desired.get(),
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      desired.detach();
      release(expected.get());
      return true;
    }

    expected = hazard()(ptr, expect);
    return false;
  }

  /**
   * \brief Compare-exchange operation.
   * \details
   * Replaces \p ptr with \p desired, if it is equal to \p expected.
   *
   * If this fails, \p expected is updated with the value stored in \p ptr.
   * \param ptr The atomic pointer to change.
   * \param expected The expected value of \p ptr.
   * \param desired The value to assign to \p ptr, if \p ptr holds \p expected.
   */
  static auto compare_exchange_strong(std::atomic<T*>& ptr, pointer& expected, pointer desired)
  noexcept
  -> bool {
    hazard hz;

    for (;;) {
      T* expect = expected.get();
      if (ptr.compare_exchange_strong(
              expect,
              desired.get(),
              std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        desired.detach();
        release(expected.get());
        return true;
      }

      auto actual = hz(ptr, expect);
      if (expected != actual) {
        expected = std::move(actual);
        return false;
      }
    }

    /* unreachable */
  }

 private:
  // Hazard data structure; aligned to not cross a page boundary,
  // thus limiting the number of TLB entries required for this to one.
  alignas(sizeof(ptr_set)) static inline ptr_set ptr_set_impl_;

  ///\brief Singleton set of pointers.
  static auto ptr_set_()
  noexcept
  -> ptr_set& {
    return ptr_set_impl_;
  }

  ///\brief Allocate a hazard store.
  static auto allocate_()
  noexcept
  -> data& {
    static std::atomic<unsigned int> seq_{ 0u };

    ptr_set& ps = ptr_set_();
    return ps[seq_.fetch_add(1u, std::memory_order_relaxed) % ps.size()];
  }

  ///\brief Acquire a reference to ptr.
  static auto acquire_(T* ptr)
  noexcept
  -> T* {
    // ADL
    if (ptr != nullptr) intrusive_ptr_add_ref(ptr);
    return ptr;
  }

  ///\brief Release a reference to ptr.
  static auto release_(T* ptr)
  noexcept
  -> void {
    // ADL
    if (ptr != nullptr) intrusive_ptr_release(ptr);
  }

  ///\brief Instance used to publish intent.
  data& d_;
};

/**
 * \brief Hazard pointer.
 * \details
 * Implements an atomic, reference-counted pointer.
 *
 * Uses the same rules as \ref intrusive_ptr with regards to
 * acquiring and releasing reference counter.
 *
 * \sa intrusive_ptr
 * \sa hazard
 */
template<typename T>
class hazard_ptr {
 private:
  ///\brief Algorithm implementation.
  using hazard_t = hazard<T>;

 public:
  ///\brief Element type of the pointer.
  using element_type = T;
  ///\brief Smart pointer equivalent.
  using pointer = typename hazard_t::pointer;
  ///\brief Type held in this atomic.
  using value_type = pointer;

#if __cplusplus >= 201703
  ///\brief Indicate if this is always a lock free implementation.
  static constexpr bool is_always_lock_free = std::atomic<T*>::is_always_lock_free;
#endif

  ///\brief Test if this instance is lock free.
  auto is_lock_free() const
  noexcept
  -> bool {
    return ptr_.is_lock_free();
  }

  ///\brief Test if this instance is lock free.
  auto is_lock_free() const volatile
  noexcept
  -> bool {
    return ptr_.is_lock_free();
  }

  ///\brief Default constructor initializes to nullptr.
  hazard_ptr() noexcept = default;

  ///\brief Copy construction.
  hazard_ptr(const hazard_ptr& p) noexcept
  : hazard_ptr(hazard_t()(p.ptr_))
  {}

  ///\brief Move construction.
  hazard_ptr(hazard_ptr&& p) noexcept
  : ptr_(p.ptr_.exchange(nullptr, std::memory_order_acq_rel))
  {}

  /**
   * \brief Initializing constructor.
   * \post
   * *this == original value of \p p.
   *
   * \post
   * \p p == nullptr
   */
  hazard_ptr(pointer&& p) noexcept
  : ptr_(p.detach())
  {}

  /**
   * \brief Initializing constructor.
   * \post
   * *this == \p p
   */
  hazard_ptr(const pointer& p) noexcept
  : hazard_ptr(pointer(p))
  {}

  ///\brief Destructor.
  ///\details Releases the held pointer.
  ~hazard_ptr() noexcept {
    reset();
  }

  /**
   * \brief Copy assignment.
   * \returns p.get()
   * \post
   * *this == \p
   */
  auto operator=(const hazard_ptr& p)
  noexcept
  -> pointer {
    return *this = p.get();
  }

  /**
   * \brief Move assignment.
   * \returns p.get()
   * \post
   * *this == original value of \p p
   *
   * \post
   * \p p == nullptr
   */
  auto operator=(hazard_ptr&& p)
  noexcept
  -> pointer {
    return *this = p.exchange(nullptr);
  }

  /**
   * \brief Copy assignment.
   * \returns \p p
   * \post
   * *this == \p p
   */
  auto operator=(const pointer& p)
  noexcept
  -> pointer {
    reset(p);
    return p;
  }

  /**
   * \brief Move assignment.
   * \returns \p p
   * \post
   * *this == original value of \p p
   *
   * \post
   * \p p == nullptr
   */
  auto operator=(pointer&& p)
  noexcept
  -> pointer {
    reset(p);
    return std::move(p);
  }

  /**
   * \brief nullptr assignment.
   * \returns pointer(nullptr)
   * \post
   * *this == nullptr
   */
  auto operator=([[maybe_unused]] const std::nullptr_t nil)
  noexcept
  -> pointer {
    reset();
    return nullptr;
  }

  /**
   * \brief Reset this.
   * \post
   * *this == nullptr
   */
  auto reset()
  noexcept
  -> void {
    hazard_t::reset(ptr_);
  }

  /**
   * \brief Reset this.
   * \post
   * *this == nullptr
   */
  auto reset([[maybe_unused]] std::nullptr_t nil)
  noexcept
  -> void {
    hazard_t::reset(ptr_);
  }

  /**
   * \brief Assignment.
   * \post
   * *this == original value of \p p
   *
   * \post
   * \p p == nullptr
   */
  auto reset(pointer&& p)
  noexcept
  -> void {
    store(std::move(p));
  }

  /**
   * \brief Assignment.
   * \post
   * *this == \p p
   */
  auto reset(const pointer& p)
  noexcept
  -> void {
    store(p);
  }

  ///\brief Automatic conversion to pointer.
  operator pointer() const noexcept {
    return get();
  }

  /**
   * \brief Read the value of this.
   * \details Marked ``[[nodiscard]]``, as there's no point in reading the
   * pointer if you're not going to evaluate it.
   * \returns Pointer in this.
   */
  [[nodiscard]]
  auto get() const
  noexcept
  -> pointer {
    return load();
  }

  /**
   * \brief Read the value of this.
   * \details Marked ``[[nodiscard]]``, as there's no point in reading the
   * pointer if you're not going to evaluate it.
   * \returns Pointer in this.
   */
  [[nodiscard]]
  auto load() const
  noexcept
  -> pointer {
    return hazard_t()(ptr_);
  }

  /**
   * \brief Assignment.
   * \post
   * *this == original value of \p p
   *
   * \post
   * \p p == nullptr
   */
  auto store(pointer&& p)
  noexcept
  -> void {
    hazard_t::reset(ptr_, std::move(p));
  }

  /**
   * \brief Assignment.
   * \post
   * *this == \p p
   */
  auto store(const pointer& p)
  noexcept
  -> void {
    hazard_t::reset(ptr_, p);
  }

  /**
   * \brief Exchange operation.
   * \param p New value of this.
   * \returns Original value of this.
   * \post
   * *this == original value of \p p
   *
   * \post
   * \p p == nullptr
   */
  [[nodiscard]]
  auto exchange(pointer&& p)
  noexcept
  -> pointer {
    return hazard_t::exchange(ptr_, std::move(p));
  }

  /**
   * \brief Exchange operation.
   * \param p New value of this.
   * \returns Original value of this.
   * \post
   * *this == \p p
   */
  [[nodiscard]]
  auto exchange(const pointer& p)
  noexcept
  -> pointer {
    return hazard_t::exchange(ptr_, p);
  }

  ///\brief Weak compare-exchange operation.
  auto compare_exchange_weak(pointer& expected, pointer desired)
  noexcept
  -> bool {
    return hazard_t::compare_exchange_weak(expected, std::move(desired));
  }

  ///\brief Strong compare-exchange operation.
  auto compare_exchange_strong(pointer& expected, pointer desired)
  noexcept
  -> bool {
    return hazard_t::compare_exchange_strong(expected, std::move(desired));
  }

  ///\brief Equality comparison.
  friend auto operator==(const hazard_ptr& x, std::nullptr_t y)
  noexcept
  -> bool {
    return x.ptr_.load(std::memory_order_acquire) == y;
  }

  ///\brief Equality comparison.
  friend auto operator==(const hazard_ptr& x, std::add_const_t<T>* y)
  noexcept
  -> bool {
    return x.ptr_.load(std::memory_order_acquire) == y;
  }

  ///\brief Equality comparison.
  template<typename U>
  friend auto operator==(const hazard_ptr& x, const intrusive_ptr<U>& y)
  noexcept
  -> std::enable_if_t<std::is_convertible_v<U*, T*>, bool> {
    return x == y.get();
  }

  ///\brief Equality comparison.
  friend auto operator==(std::nullptr_t x, const hazard_ptr& y)
  noexcept
  -> bool {
    return y == x;
  }

  ///\brief Equality comparison.
  friend auto operator==(std::add_const_t<T>* x, const hazard_ptr& y)
  noexcept
  -> bool {
    return y == x;
  }

  ///\brief Equality comparison.
  template<typename U>
  friend auto operator==(const intrusive_ptr<U>& x, const hazard_ptr& y)
  noexcept
  -> std::enable_if_t<std::is_convertible_v<U*, T*>, bool> {
    return y == x;
  }

  ///\brief Inequality comparison.
  friend auto operator!=(const hazard_ptr& x, std::nullptr_t y)
  noexcept
  -> bool {
    return !(x == y);
  }

  ///\brief Inequality comparison.
  friend auto operator!=(const hazard_ptr& x, std::add_const_t<T>* y)
  noexcept
  -> bool {
    return !(x == y);
  }

  ///\brief Inequality comparison.
  template<typename U>
  friend auto operator!=(const hazard_ptr& x, const intrusive_ptr<U>& y)
  noexcept
  -> std::enable_if_t<std::is_convertible_v<U*, T*>, bool> {
    return !(x == y);
  }

  ///\brief Inequality comparison.
  friend auto operator!=(std::nullptr_t x, const hazard_ptr& y)
  noexcept
  -> bool {
    return !(x == y);
  }

  ///\brief Inequality comparison.
  friend auto operator!=(std::add_const_t<T>* x, const hazard_ptr& y)
  noexcept
  -> bool {
    return !(x == y);
  }

  ///\brief Inequality comparison.
  template<typename U>
  friend auto operator!=(const intrusive_ptr<U>& x, const hazard_ptr& y)
  noexcept
  -> std::enable_if_t<std::is_convertible_v<U*, T*>, bool> {
    return !(x == y);
  }

 private:
  ///\brief Internally used atomic pointer.
  std::atomic<T*> ptr_ = nullptr;
};


/**
 * \brief Colours used by the GC algorithm.
 * \details
 * The GC algorithm is designed to allow pointer manipulation
 * while the GC is active.
 *
 * Red pointer manipulation rules are:
 * 1. *Red-promotion*: When a red element has its reference counter incremented from 0 to 1, it must become grey.
 * 2. *Red-demotion*: Done by the GC in its initial phase. May not be performed outside the GC.
 * 3. *Red-promotion* is allowed when the pointee is known reachable (non-weak pointer reads).
 * 4. *Red-promotion* is allowed for weak pointer reads, only when locking out the GC.
 * 5. When a reference changes from 1 to 0, it must be followed by a call to the GC, unless it is provably reachable.
 *
 * Furthermore:
 * 6. Only the GC is allowed to change red nodes to black.
 * 7. The GC is responsible for calling the destruction of black pointees.
 * 8. Only the GC is allowed to change nodes to a red colour. (Rules 2 and 5.)
 *
 * Invariants:
 * 1. A pointer with 1 or more references, shall be white or grey.
 * 2. A pointer which is reachable shall not be black.
 * 3. An unreachable pointer shall be red or black.
 * 4. A black pointer is unreachable (and consequently has 0 references).
 *
 * Note that the GC algorithm is the only one that distinguishes between
 * white and grey colours.
 */
enum class color : std::uintptr_t {
  red = 0, ///< May or may not be reachable.
  black = 1, ///< Unreachable.
  grey = 2, ///< Reachable, during GC: may point at zero or more red edges.
  white = 3, ///< Reachable, during GC: points at no red edges.
};

/**
 * \brief Number of lower bits used to track colour.
 * \details The reference count is shifted up by this amount of bits.
 */
constexpr unsigned int color_shift = 2;

/**
 * \brief Mask bits for reading colour from reference.
 */
constexpr std::uintptr_t color_mask = (1u << color_shift) - 1u;

/**
 * \brief Extract number of references from reference counter.
 */
constexpr auto get_refs(std::uintptr_t refcounter)
noexcept
-> std::uintptr_t {
  return (refcounter & ~color_mask) >> color_shift;
}

/**
 * \brief Extract color from reference counter.
 */
constexpr auto get_color(std::uintptr_t refcounter)
noexcept
-> color {
  return color(refcounter & color_mask);
}

/**
 * \brief Create reference counter with given number of references and color.
 */
constexpr auto make_refcounter(std::uintptr_t nrefs, color c)
noexcept
-> std::uintptr_t {
  return (nrefs << color_shift) | static_cast<std::uintptr_t>(c);
}

/**
 * \brief Color invariant for reference counter.
 */
constexpr auto color_invariant(std::uintptr_t refcounter)
noexcept
-> bool {
  return
      (get_refs(refcounter) > 0u
       ?  get_color(refcounter) == color::white || get_color(refcounter) == color::grey
       : true)
      && (get_color(refcounter) == color::black
          ? get_refs(refcounter) == 0u
          : true);
}


class vertex
: public link<vertex>
{
  friend class generation;
  friend class base_control;

 protected:
  vertex();

  vertex(const vertex& other [[maybe_unused]])
  : vertex()
  {}

  explicit vertex(intrusive_ptr<base_control> bc) noexcept;
  ~vertex() noexcept;

  auto reset() noexcept -> void;

  /**
   * \brief Assign a new \p new_dst.
   * \details
   * Assigns \p new_dst as the pointee of this vertex.
   *
   * \param new_dst The destination of the edge. May be null.
   * \param has_reference If set, \p new_dst has a reference count that will be consumed.
   * (Ignored for null new_dst.)
   * \param no_red_promotion If set, caller guarantees no red promotion is required.
   * Note that this must be set, if has_reference is set.
   * (Ignored for null new_dst.)
   */
  auto reset(
      intrusive_ptr<base_control> new_dst,
      bool has_reference,
      bool no_red_promotion) noexcept
  -> void;

  ///\brief Test if origin is expired.
  auto owner_is_expired() const noexcept -> bool;

 public:
  ///\brief Read the target control block.
  ///\returns The target control block of this vertex.
  auto get_control() const noexcept -> intrusive_ptr<base_control>;

 private:
  const intrusive_ptr<base_control> bc_; // Non-null.
  hazard_ptr<base_control> dst_;
};


/**
 * \brief Base class for all control blocks.
 * \details
 * Contains all variables required for the algorithm to function.
 */
class base_control
: public link<base_control>
{
  friend class generation;
  friend class vertex;
  template<typename> friend class cycle_ptr::cycle_allocator;

  ///\brief Increment reference counter.
  ///\details Used by intrusive_ptr.
  friend auto intrusive_ptr_add_ref(base_control* bc)
  noexcept
  -> void {
    assert(bc != nullptr);

    [[maybe_unused]]
    std::uintptr_t old = bc->control_refs_.fetch_add(1u, std::memory_order_acquire);
    assert(old > 0u && old < UINTPTR_MAX);
  }

  ///\brief Decrement reference counter.
  ///\details Used by intrusive_ptr.
  ///Destroys this if the last reference goes away.
  friend auto intrusive_ptr_release(base_control* bc)
  noexcept
  -> void {
    assert(bc != nullptr);

    std::uintptr_t old = bc->control_refs_.fetch_sub(1u, std::memory_order_release);
    assert(old > 0u);

    if (old == 1u) std::invoke(bc->get_deleter_(), bc);
  }

  base_control(const base_control&) = delete;

 protected:
  class publisher;

  ///\brief Default constructor allocates a new generation.
  base_control();
  ///\brief Constructor to use a specific generation.
  base_control(intrusive_ptr<generation> g) noexcept;
  ///\brief Destructor.
  ~base_control() noexcept;

 public:
  ///\brief Create a control block that represents no ownership.
  static auto unowned_control() -> intrusive_ptr<base_control>;

  ///\brief Test if the object managed by this control is expired.
  auto expired() const
  noexcept
  -> bool {
    return get_color(store_refs_.load(std::memory_order_relaxed)) == color::black;
  }

  ///\brief Implements publisher lookup based on address range.
  static auto publisher_lookup(void* addr, std::size_t len) -> intrusive_ptr<base_control>;

  ///\brief Used by weak to strong reference promotion.
  ///\return True if promotion succeeded, false otherwise.
  auto weak_acquire() noexcept -> bool;

  /**
   * \brief Acquire reference.
   * \details
   * May only be called when it is certain there won't be red promotion
   * involved (meaning, it's certain that the reference counter is
   * greater than zero).
   *
   * Faster than acquire().
   */
  auto acquire_no_red()
  noexcept
  -> void {
    [[maybe_unused]]
    std::uintptr_t old = store_refs_.fetch_add(1u << color_shift, std::memory_order_relaxed);
    assert(get_color(old) != color::black && get_color(old) != color::red);
  }

  /**
   * \brief Acquire reference.
   * \details
   * Increments the reference counter.
   *
   * May only be called on reachable instances of this.
   */
  auto acquire() noexcept -> void;

  /**
   * \brief Release reference counter.
   * \details
   * Reference counter is decremented by one.
   * If the reference counter drops to zero, a GC invocation will be performed,
   * unless \p skip_gc is set.
   *
   * \param skip_gc Flag to suppress GC invocation. May only be set to true if
   * caller can guarantee that this is live.
   */
  auto release(bool skip_gc = false)
  noexcept
  -> void {
    const std::uintptr_t old = store_refs_.fetch_sub(
        1u << color_shift,
        std::memory_order_release);
    assert(get_refs(old) > 0u);

    if (!skip_gc && get_refs(old) == 1u) gc();
  }

  /**
   * \brief Run GC.
   */
  auto gc() noexcept -> void;

  ///\brief Register a vertex.
  auto push_back(vertex& v)
  noexcept
  -> void {
    std::lock_guard<std::mutex> lck{ mtx_ };
    edges_.push_back(v);
  }

  ///\brief Deregister a vertex.
  auto erase(vertex& v)
  noexcept
  -> void {
    std::lock_guard<std::mutex> lck{ mtx_ };
    edges_.erase(edges_.iterator_to(v));
  }

  ///\brief Test if this control block represents an unowned object.
  virtual auto is_unowned() const noexcept -> bool;

 private:
  ///\brief Destroy object managed by this control block.
  virtual auto clear_data_() noexcept -> void = 0;
  /**
   * \brief Retrieve the deleter function.
   * \details
   * Deleter is a free function, because control blocks need to use their
   * construction allocator to erase.
   */
  virtual auto get_deleter_() const noexcept -> void (*)(base_control*) noexcept = 0;

  ///\brief Reference counter on managed object.
  ///\details Initially has a value of 1.
  std::atomic<std::uintptr_t> store_refs_{ make_refcounter(1u, color::white) };
  ///\brief Reference counter on control block.
  ///\details Initially has a value of 1.
  std::atomic<std::uintptr_t> control_refs_{ std::uintptr_t(1) };
  ///\brief Pointer to generation.
  hazard_ptr<generation> generation_;
  ///\brief Mutex to protect edges.
  std::mutex mtx_;
  ///\brief List of edges originating from object managed by this control block.
  llist<vertex, vertex> edges_;

 public:
  /**
   * \brief This variable indicates the managed object is under construction.
   * \details
   * It is used to prevent \ref base_control::shared_from_this from
   * handing out references until construction has completed.
   *
   * If a pointer was handed out before, we would get into a difficult scenario
   * where the constructor of an object could publish itself and then fail
   * its construction later.
   * At which point we would have the difficult question of how to manage
   * a dangling pointer.
   *
   * By preventing this, we prevent failed constructors from accidentally
   * publishing an uninitialized pointer.
   */
  bool under_construction = true;
};


/**
 * \brief Address range publisher.
 * \details
 * Publishes that a range of memory is managed by a specific control block,
 * so that \ref base_control and \ref cycle_member_ptr can use automatic
 * deduction of ownership.
 */
class base_control::publisher {
 private:
  ///\brief Address range.
  struct address_range {
    ///\brief Base memory address.
    void* addr;
    ///\brief Size of memory in bytes.
    std::size_t len;

    ///\brief Equality comparison.
    auto operator==(const address_range& other) const
    noexcept
    -> bool {
      return std::tie(addr, len) == std::tie(other.addr, other.len);
    }

    ///\brief Inequality comparison.
    auto operator!=(const address_range& other) const
    noexcept
    -> bool {
      return !(*this == other);
    }

    ///\brief Less operator for use in map.
    auto operator<(const address_range& other) const
    noexcept
    -> bool {
      return addr < other.addr;
    }
  };

  /**
   * \brief Map of address ranges.
   * \details
   * This is an ordered map, sorted by address (ascending).
   *
   * It must be an ordered map, since the lookup queries will likely contain
   * sub-ranges.
   *
   * \note
   * We use a global map for published memory ranges, rather than a TLS variable.
   * \par
   * This is because a TLS variable would break, if the called constructor,
   * during our publishing stage, would jump threads.
   * This is very possible to happen both with boost::asio and the upcoming
   * co-routine additions in C++20.
   * For example, if a co-routine creates an object, which during its
   * construction invokes an awaitable function.
   */
  using map_type = std::map<address_range, base_control*>;

  publisher() = delete;
  publisher(const publisher&) = delete;

 public:
  ///\brief Publish a base_control for an object at the given address.
  publisher(void* addr, std::size_t len, base_control& bc);
  ///\brief Destructor, unpublishes the range.
  ~publisher() noexcept;

  ///\brief Perform a lookup, to figure out which control manages the given address range.
  ///\details Finds the base_control for which a publisher is active.
  ///The range is usually smaller than the range managed by the control.
  ///\param[in] addr Object offset for which to find a base_control.
  ///\param[in] len Sizeof the object for which to find a base control.
  ///\returns Base control owning the argument address range.
  ///\throws std::runtime_error if no pushlished range covers the argument range.
  static auto lookup(void* addr, std::size_t len) -> intrusive_ptr<base_control>;

 private:
  /**
   * \brief Global map of ranges.
   * \details The singleton map maintains all published ranges.
   *
   * It is a map, instead of a TLS pointer, as the latter might be wrong, when
   * an object constructor calls a method implemented as a co-routine.
   * In the case of (boost) asio, this could cause the constructor to switch
   * threads and thus make the pointer invisible.
   *
   * \returns Map for range publication, with its associated mutex.
   */
  static auto singleton_map_() noexcept
  -> std::tuple<std::shared_mutex&, map_type&>;

  ///\brief Iterator into published data.
  map_type::const_iterator iter_;
};


inline auto base_control::publisher_lookup(void* addr, std::size_t len)
-> intrusive_ptr<base_control> {
  return publisher::lookup(addr, len);
}


class generation {
  friend cycle_ptr::gc_operation;

  friend auto intrusive_ptr_add_ref(generation* g)
  noexcept
  -> void {
    assert(g != nullptr);

    [[maybe_unused]]
    std::uintptr_t old = g->refs_.fetch_add(1u, std::memory_order_relaxed);
    assert(old < UINTPTR_MAX);
  }

  friend auto intrusive_ptr_release(generation* g)
  noexcept
  -> void {
    assert(g != nullptr);

    std::uintptr_t old = g->refs_.fetch_sub(1u, std::memory_order_release);
    assert(old > 0u);

    if (old == 1u) delete g;
  }

 private:
  using controls_list = llist<base_control, base_control>;

  generation() = default;

  generation(std::uintmax_t seq)
  : seq_(seq)
  {}

  generation(const generation&) = delete;

#ifndef NDEBUG
  ~generation() noexcept {
    assert(controls_.empty());
    assert(refs_ == 0u);
  }
#else
  ~generation() noexcept = default;
#endif

 public:
  static auto new_generation()
  -> intrusive_ptr<generation> {
    return intrusive_ptr<generation>(new generation(), true);
  }

  static auto new_generation(std::uintmax_t seq)
  -> intrusive_ptr<generation> {
    return intrusive_ptr<generation>(new generation(seq), true);
  }

  static auto order_invariant(const generation& origin, const generation& dest)
  noexcept
  -> bool {
    return origin.seq() < (dest.seq() & ~moveable_seq);
  }

  auto link(base_control& bc) noexcept
  -> void {
    std::lock_guard<std::shared_mutex> lck{ mtx_ };
    controls_.push_back(bc);
  }

  auto unlink(base_control& bc) noexcept
  -> void {
    std::lock_guard<std::shared_mutex> lck{ mtx_ };
    controls_.erase(controls_.iterator_to(bc));
  }

 private:
  static constexpr std::uintmax_t moveable_seq = 0x1;

  static auto new_seq_() noexcept -> std::uintmax_t;

 public:
  auto seq() const
  noexcept
  -> std::uintmax_t {
    return seq_.load(std::memory_order_relaxed);
  }

  auto gc() noexcept -> void;

  /**
   * \brief Ensure src and dst meet constraint, in order to
   * create an edge between them.
   * \details
   * Ensures that either:
   * 1. order_invariant holds between generation in src and dst; or
   * 2. src and dst are in the same generation.
   *
   * \returns A lock to hold while creating the edge.
   */
  static auto fix_ordering(base_control& src, base_control& dst) noexcept
  -> std::shared_lock<std::shared_mutex>;

 private:
  /**
   * \brief Single run of the GC.
   * \details Performs the GC algorithm.
   *
   * The GC runs in two mark-sweep algorithms, in distinct phases.
   * - Phase 1: operates mark-sweep, but does not lock out other threads.
   * - Phase 2: operates mark-sweep on the remainder, locking out other threads attempting weak-pointer acquisition.
   * - Phase 3: colour everything still not reachable black.
   * - Destruction phase: this phase runs after phase 3, with all GC locks unlocked.
   *   It is responsible for destroying the pointees.
   *   It may, consequently, trip GCs on other generations;
   *   hence why we *must* run it unlocked, otherwise we would get
   *   inter generation lock ordering problems.
   */
  auto gc_() noexcept -> void;

  /**
   * \brief Mark phase of the GC algorithm.
   * \details Marks all white nodes red or grey, depending on their reference
   * counter.
   *
   * Partitions controls_ according to the initial mark predicate.
   * \returns Partition end iterator.
   * All elements before the returned iterator are known reachable,
   * but haven't had their edges processed.
   * All elements after the returned iterator may or may not be reachable.
   */
  auto gc_mark_() noexcept -> controls_list::iterator;

  /**
   * \brief Phase 2 mark
   * \details
   * Extends the wavefront in ``[controls_.begin(), b)`` with anything
   * non-red after \p b.
   */
  auto gc_phase2_mark_(controls_list::iterator b) noexcept
  -> controls_list::iterator;

  /**
   * \brief Sweep phase of the GC algorithm.
   * \details Takes the wavefront and for each element,
   * adds all outgoing links to the wavefront.
   *
   * Processed grey elements are marked white.
   * \returns Partition end iterator.
   * Elements before the iterator are known reachable.
   * Elements after are not reachable (but note that red-promotion may make them reachable).
   */
  auto gc_sweep_(controls_list::iterator wavefront_end) noexcept
  -> controls_list::iterator;

  /**
   * \brief Perform phase 2 mark-sweep.
   * \details
   * In phase 2, weak red promotion is no longer allowed.
   * \returns End partition iterator of reachable set.
   */
  auto gc_phase2_sweep_(controls_list::iterator wavefront_end) noexcept
  -> controls_list::iterator;

  /**
   * \brief Merge two generations.
   * \details
   * Pointers in \p src and \p dst must be distinct,
   * and \p dst must not precede \p src.
   *
   * Also, if \p src and \p dst have the same sequence,
   * address of \p src must be before address of \p dst.
   * \param x,y Generations to merge.
   * \returns Pointer to the merged generation.
   */
  static auto merge_(
      std::tuple<intrusive_ptr<generation>, bool> src_tpl,
      std::tuple<intrusive_ptr<generation>, bool> dst_tpl) noexcept
  -> std::tuple<intrusive_ptr<generation>, bool>;

  /**
   * \brief Low level merge operation.
   * \details
   * Moves all elements from \p x into \p y, leaving \p x empty.
   *
   * May only be called when no other generations are sequenced between \p x and \p y.
   *
   * The GC promise (second tuple element) of a generation is fulfilled on
   * the generation that is drained of elements, by propagating it
   * the result.
   *
   * \p x must precede \p y.
   *
   * \p x must be locked for controls_, GC (via controls_) and merge_.
   * \param x,y The two generations to merge together, together with
   * predicate indicating if GC is promised by this thread.
   * Note that the caller must have (shared) ownership of both generations.
   * \param x_mtx_lock Lock on \p x, used for validation only.
   * \param x_mtx_lock Lock on merges from \p x, used for validation only.
   * \returns True if \p y needs to be GC'd by caller.
   */
  [[nodiscard]]
  static auto merge0_(
      std::tuple<generation*, bool> x,
      std::tuple<generation*, bool> y,
      const std::unique_lock<std::shared_mutex>& x_mtx_lck [[maybe_unused]],
      const std::unique_lock<std::shared_mutex>& x_merge_mtx_lck [[maybe_unused]]) noexcept
  -> bool;

 public:
  ///\brief Mutex protecting controls_ and GC.
  std::shared_mutex mtx_;
  ///\brief Mutex protecting merges.
  ///\note ``merge_mtx_`` must be acquired before ``mtx_``.
  std::shared_mutex merge_mtx_;

 private:
  ///\brief All controls that are part of this generation.
  controls_list controls_;

 public:
  ///\brief Lock to control weak red-promotions.
  ///\details
  ///When performing a weak red promotion, this lock must be held for share.
  ///The GC will hold this lock exclusively, to prevent red promotions.
  ///
  ///Note that this lock is not needed when performing a strong red-promotion,
  ///as the promoted element is known reachable, thus the GC would already have
  ///processed it during phase 1.
  std::shared_mutex red_promotion_mtx_;

 private:
  ///\brief Sequence number of this generation.
  std::atomic<std::uintmax_t> seq_ = new_seq_();
  ///\brief Reference counter for intrusive_ptr.
  std::atomic<std::uintptr_t> refs_{ 0u };
  ///\brief Flag indicating a pending GC.
  std::atomic_flag gc_flag_;
};


/**
 * \brief Control block implementation for given type and allocator combination.
 * \details
 * Extends base_control, adding specifics for creating and destroying \p T.
 *
 * \tparam T The type of object managed by this control block.
 * \tparam Alloc The allocator used to allocate storage for this control block.
 */
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
  ///\brief Create control block.
  ///\param alloc The allocator used to allocate space for this control block.
  control(Alloc alloc)
  : Alloc(std::move(alloc))
  {}

  ///\brief Instantiate the object managed by this control block.
  ///\details
  ///Uses placement new to instantiate the object that is being managed.
  ///\pre !this->under_construction
  ///\post this->under_construction
  ///\param args Arguments to pass to constructor of \p T.
  ///\throws ... if constructor of \p T throws.
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
  ///\brief Destroy object.
  ///\pre this has a constructed object (i.e. a successful call to \ref instantiate).
  ///\note May not clear this->under_construction, due to assertions in base_control destructor.
  auto clear_data_()
  noexcept
  -> void override {
    assert(!this->under_construction);
    reinterpret_cast<T*>(&store_)->~T();
  }

  ///\brief Get function that performs deletion of this.
  ///\returns A function that, when passed this, will destroy this.
  auto get_deleter_() const
  noexcept
  -> void (*)(base_control* bc) noexcept override {
    return &deleter_impl_;
  }

  ///\brief Implementation of delete function.
  ///\details Uses allocator supplied at construction to destroy and deallocate this.
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

  ///\brief Storage for managed object.
  std::aligned_storage_t<sizeof(T), alignof(T)> store_;
};


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


inline base_control::base_control()
: base_control(generation::new_generation())
{}

inline base_control::base_control(intrusive_ptr<generation> g) noexcept {
  assert(g != nullptr);
  g->link(*this);
  generation_.reset(std::move(g));
}

inline base_control::~base_control() noexcept {
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

inline auto base_control::unowned_control()
-> intrusive_ptr<base_control> {
  return intrusive_ptr<base_control>(new unowned_control_impl(), false);
}

inline auto base_control::weak_acquire()
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

inline auto base_control::acquire()
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

inline auto base_control::gc()
noexcept
-> void {
  intrusive_ptr<generation> gen_ptr;
  do {
    gen_ptr = generation_.get();
    gen_ptr->gc();
  } while (gen_ptr != generation_);
}

inline auto base_control::is_unowned() const
noexcept
-> bool {
  return false;
}


inline base_control::publisher::publisher(void* addr, std::size_t len, base_control& bc) {
  const auto mtx_and_map = singleton_map_();
  std::lock_guard<std::shared_mutex> lck{ std::get<std::shared_mutex&>(mtx_and_map) };

  [[maybe_unused]]
  bool success;
  std::tie(iter_, success) =
      std::get<map_type&>(mtx_and_map).emplace(address_range{ addr, len }, &bc);

  assert(success);
}

inline base_control::publisher::~publisher() noexcept {
  const auto mtx_and_map = singleton_map_();
  std::lock_guard<std::shared_mutex> lck{ std::get<std::shared_mutex&>(mtx_and_map) };

  std::get<map_type&>(mtx_and_map).erase(iter_);
}

inline auto base_control::publisher::lookup(void* addr, std::size_t len)
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

inline auto base_control::publisher::singleton_map_()
noexcept
-> std::tuple<std::shared_mutex&, map_type&> {
  static std::shared_mutex mtx;
  static map_type map;
  return std::tie(mtx, map);
}


} /* namespace cycle_ptr::detail */


namespace cycle_ptr {


/**
 * \brief GC operations for delayed collection.
 * \details
 * This functor invokes a garbage collect for a given generation.
 *
 * Multiple invocations of this functor are idempotent.
 *
 * \attention Should be invoked at least once, or there is a risk of
 * leaking memory.
 */
class gc_operation {
 public:
  ///\brief Default constructor performs no collections.
  constexpr gc_operation() noexcept = default;

  ///\brief Constructor for internal use.
  explicit gc_operation(detail::intrusive_ptr<detail::generation> g) noexcept
  : g_(std::move(g))
  {}

  ///\brief Run the GC.
  ///\details Multiple calls to this method are idempotent.
  auto operator()()
  noexcept
  -> void {
    if (g_ != nullptr) g_->gc_();
    g_.reset();
  }

 private:
  ///\brief The generation on which to run a GC.
  detail::intrusive_ptr<detail::generation> g_;
};


} /* namespace cycle_ptr */


namespace cycle_ptr::detail {


struct delay_gc_impl_ {
  std::shared_mutex mtx;
  delay_gc fn;

  static auto singleton()
  -> delay_gc_impl_& {
    static delay_gc_impl_ impl;
    return impl;
  }
};


inline auto maybe_delay_gc_(detail::generation& g)
-> bool {
  try {
    delay_gc_impl_& impl = delay_gc_impl_::singleton();
    std::shared_lock<std::shared_mutex> lck{ impl.mtx };
    if (impl.fn == nullptr) return false;
    impl.fn(gc_operation(detail::intrusive_ptr<detail::generation>(&g, true)));
    return true;
  } catch (...) {
    return false;
  }
}


} /* namespace cycle_ptr::detail */


namespace cycle_ptr {


inline auto get_delay_gc()
-> delay_gc {
  detail::delay_gc_impl_& impl = detail::delay_gc_impl_::singleton();
  std::shared_lock<std::shared_mutex> lck{ impl.mtx };
  return impl.fn;
}

inline auto set_delay_gc(delay_gc f)
-> delay_gc {
  detail::delay_gc_impl_& impl = detail::delay_gc_impl_::singleton();
  std::lock_guard<std::shared_mutex> lck{ impl.mtx };
  return std::exchange(impl.fn, std::move(f));
}


} /* namespace cycle_ptr */


namespace cycle_ptr::detail {


/*
 * Sequence number generation.
 * Special value 0 is reserved for the 'unowned' generation.
 *
 * We use the low bit to indicate if a generation can have its sequence
 * number altered.
 *
 * Consequently, we start at number 2, so the 'unowned' generation can share seq 0
 * without ever getting merged and we're not using the low bit.
 * We also advance with a step size of 2, for this reason.
 *
 * By allowing the sequence number to be decremented, we can update
 * sequence numbers for RAII style acquisition, instead of requiring
 * potentially large number of merges.
 * For RAII style acquisition, the pointers of an object are likely
 * filled with values allocated earlier, thus with lower sequence numbers.
 *
 * By starting well above 2, we allow for some sequence number shifting
 * for destination generations created early at program startup.
 */
inline std::atomic<std::uintmax_t> new_seq_state{ 1002u };


inline auto generation::new_seq_()
noexcept
-> std::uintmax_t {
  // Increment by 2, see explanation at new_seq_state for why.
  const std::uintmax_t result =
      new_seq_state.fetch_add(2u, std::memory_order_relaxed)
      | moveable_seq;

  // uintmax_t will be at least 64 bit.
  // If allocating a new generation each nano second,
  // we would run out of sequence numbers after ~292 years.
  // (584 years if we didn't use a step size of 2.)
  //
  // Note that the algorithm still does the right thing when sequence numbers
  // wrap around.
  // But it could mean a giant performance penalty due to merging into
  // surviving high-sequence generations.
  assert(result != UINTPTR_MAX); // We ran out of sequence numbers.

  return result;
}

inline auto generation::gc()
noexcept
-> void {
  if (!gc_flag_.test_and_set(std::memory_order_release)) {
    if (!maybe_delay_gc_(*this)) gc_();
  }
}

inline auto generation::fix_ordering(base_control& src, base_control& dst)
noexcept
-> std::shared_lock<std::shared_mutex> {
  auto src_gen = src.generation_.load(),
       dst_gen = dst.generation_.load();
  bool dst_gc_requested = false;

  std::shared_lock<std::shared_mutex> src_merge_lck{ src_gen->merge_mtx_ };
  for (;;) {
    if (src_gen != dst_gen) // Clear movable bit in dst_gen.
      dst_gen->seq_.fetch_and(~moveable_seq, std::memory_order_relaxed);

    if (src_gen == dst_gen
        || order_invariant(*src_gen, *dst_gen)
        || (src_gen->seq() & moveable_seq) == moveable_seq) [[unlikely]] {
      while (src_gen != src.generation_) [[unlikely]] {
        src_merge_lck.unlock();
        src_gen = src.generation_.load();
        src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
      }

      // Maybe alter the sequence number.
      // We prevent the reduced generation value from going below 3:
      // - 3 & ~moveable_seq == 2, which is above generation 0 (used for unowned)
      // - if we let it go to 1, the counter could end up becoming 0,
      //   which could lead to unowned object generation being merged in,
      //   causing potentially very expensive GC invocations, if there are a lot
      //   of unowned objects.
      if (src_gen != dst_gen && !order_invariant(*src_gen, *dst_gen)) {
        std::uintmax_t src_gen_seq = src_gen->seq_.load(std::memory_order_relaxed);
        const std::uintmax_t dst_gen_seq = dst_gen->seq();
        while ((src_gen_seq & moveable_seq) == moveable_seq && dst_gen_seq > 3u) {
          assert((dst_gen_seq & moveable_seq) == 0u);
          if (src_gen->seq_.compare_exchange_weak(
                  src_gen_seq,
                  dst_gen_seq - 1u,
                  std::memory_order_relaxed,
                  std::memory_order_relaxed))
            break;
        }
      }

      if (src_gen == dst_gen || order_invariant(*src_gen, *dst_gen)) [[likely]] {
        break; // break out of for(;;) loop
      }
    }
    src_merge_lck.unlock();

    bool src_gc_requested = false;
    if (src_gen->seq() == dst_gen->seq() && dst_gen > src_gen) {
      dst_gen.swap(src_gen);
      std::swap(src_gc_requested, dst_gc_requested);
    }

    std::tie(dst_gen, dst_gc_requested) = merge_(
        std::make_tuple(std::move(dst_gen), std::exchange(dst_gc_requested, false)),
        std::make_tuple(src_gen, std::exchange(src_gc_requested, false)));

    // Update dst_gen, in case another merge moved dst away from under us.
    if (dst_gen != dst.generation_) [[unlikely]] {
      if (std::exchange(dst_gc_requested, false)) dst_gen->gc_();
      dst_gen = dst.generation_;
    }

    // Update src_gen, in case another merge moved src away from under us.
    assert(!src_gc_requested);
    assert(!src_merge_lck.owns_lock());
    if (src_gen != src.generation_) [[unlikely]] {
      src_gen = src.generation_.load();
      src_merge_lck = std::shared_lock<std::shared_mutex>{ src_gen->merge_mtx_ };
    } else {
      src_merge_lck.lock();
    }
  }

  // Validate post condition.
  assert(src_gen == src.generation_);
  assert(src_merge_lck.owns_lock() && src_merge_lck.mutex() == &src_gen->merge_mtx_);
  assert(src_gen == dst.generation_.load()
      || order_invariant(*src_gen, *dst.generation_.load()));

  if (dst_gc_requested) dst_gen->gc_();
  return src_merge_lck;
}

inline auto generation::gc_()
noexcept
-> void {
  controls_list unreachable;

  // Lock scope.
  {
    // ----------------------------------------
    // Locks for phase 1:
    // mtx_ grants write access to controls_ (needed for partitioning mark-sweep algorithm).
    // mtx_ also locks out concurrent GCs, doubling as the critical section for a GC.
    // mtx_ also protects this generation against merging, as that is a controls_ mutating operation.
    //
    // Note that we don't yet block weak red-promotion.
    // All reads on weak pointers, will act as if they happened-before the GC
    // ran and thus as if they happened before the last reference to their
    // data went away.
    std::lock_guard<std::shared_mutex> lck{ mtx_ };

    // Clear GC request flag, signalling that GC has started.
    // (We do this after acquiring initial locks, so that multiple threads can
    // forego their GC invocation.)
    gc_flag_.clear(std::memory_order_seq_cst);

    // Prepare (mark phase).
    controls_list::iterator wavefront_end = gc_mark_();
    if (wavefront_end == controls_.end()) return; // Everything is reachable.

    // Sweep phase.
    controls_list::iterator sweep_end = gc_sweep_(std::move(wavefront_end));
    if (sweep_end == controls_.end()) return; // Everything is reachable.

    // ----------------------------------------
    // Locks for phase 2:
    // exclusive lock on red_promotion_mtx_, prevents weak red-promotions.
    std::lock_guard<std::shared_mutex> red_promotion_lck{ red_promotion_mtx_ };

    // Process marks for phase 2.
    // Ensures that all grey elements in sweep_end, controls_.end() are moved into the wave front.
    wavefront_end = gc_phase2_mark_(std::move(sweep_end));
    if (wavefront_end == controls_.end()) return; // Everything is reachable.

    // Perform phase 2 sweep.
    controls_list::iterator reachable_end = gc_phase2_sweep_(std::move(wavefront_end));
    if (reachable_end == controls_.end()) return; // Everything is reachable.

    // ----------------------------------------
    // Phase 3: mark unreachables black and add a reference to their controls.
    // The range reachable_end, controls_.end(), contains all unreachable elements.
    std::for_each(
        reachable_end, controls_.end(),
        [](base_control& bc) {
          // Acquire ownership of control blocks for unreachable list.
          intrusive_ptr_add_ref(&bc); // ADL

          // Colour change.
          [[maybe_unused]]
          const auto old = bc.store_refs_.exchange(make_refcounter(0u, color::black), std::memory_order_release);
          assert(get_refs(old) == 0u && get_color(old) == color::red);
        });

    // Move to unreachable list, so we can release all GC locks.
    unreachable.splice(unreachable.end(), controls_, reachable_end, controls_.end());
  } // End of lock scope.

  // ----------------------------------------
  // Destruction phase: destroy data in each control block.
  // Clear edges in unreachable pointers.
  std::for_each(
      unreachable.begin(), unreachable.end(),
      [this](base_control& bc) {
        std::lock_guard<std::mutex> lck{ bc.mtx_ }; // Lock edges_
        for (vertex& v : bc.edges_) {
          intrusive_ptr<base_control> dst = v.dst_.exchange(nullptr);
          if (dst != nullptr && dst->generation_ != this)
            dst->release(); // Reference count decrement.
        }
      });

  // Destroy unreachables.
  while (!unreachable.empty()) {
    // Transfer ownership from unreachable list to dedicated pointer.
    const auto bc_ptr = intrusive_ptr<base_control>(
        &unreachable.pop_front(),
        false);

    bc_ptr->clear_data_(); // Object destruction.
  }

  // And we're done. :)
}

inline auto generation::gc_mark_()
noexcept
-> controls_list::iterator {
  // Create wavefront.
  // Element colors:
  // - WHITE -- strongly reachable.
  // - BLACK -- unreachable.
  // - GREY -- strongly reachable, but referents need color update.
  // - RED -- not strongly reachable, but may or may not be reachable.
  controls_list::iterator wavefront_end = controls_.begin();

  controls_list::iterator i = controls_.begin();
  while (i != controls_.end()) {
    std::uintptr_t expect = make_refcounter(0u, color::white);
    for (;;) {
      assert(get_color(expect) != color::black);
      const color target_color = (get_refs(expect) == 0u ? color::red : color::grey);
      if (i->store_refs_.compare_exchange_weak(
              expect,
              make_refcounter(get_refs(expect), target_color),
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        if (target_color == color::red) {
          ++i;
        } else if (i == wavefront_end) {
          ++wavefront_end;
          ++i;
        } else {
          controls_.splice(wavefront_end, controls_, i++);
        }
        break;
      } else if (get_color(expect) == color::red) {
        ++i;
        break;
      }
    }
  }

  return wavefront_end;
}

inline auto generation::gc_phase2_mark_(controls_list::iterator b)
noexcept
-> controls_list::iterator {
  controls_list::iterator wavefront_end = b;

  while (b != controls_.end()) {
    const color b_color =
        get_color(b->store_refs_.load(std::memory_order_acquire));
    assert(b_color == color::grey || b_color == color::red);

    if (b_color == color::red) {
      ++b;
    } else if (b == wavefront_end) {
      ++wavefront_end;
      ++b;
    } else {
      controls_.splice(wavefront_end, controls_, b++);
    }
  }

  return wavefront_end;
}

inline auto generation::gc_sweep_(controls_list::iterator wavefront_end)
noexcept
-> controls_list::iterator {
  controls_list::iterator wavefront_begin = controls_.begin();

  while (wavefront_begin != wavefront_end) {
    {
      // Promote grey to white.
      // Note that if the colour isn't grey, another thread may have performed
      // red-demotion on this entry and will be queueing for GC.
      std::uintptr_t expect = wavefront_begin->store_refs_.load(std::memory_order_relaxed);
      for (;;) {
        assert(get_color(expect) == color::grey || get_color(expect) == color::red);
        if (wavefront_begin->store_refs_.compare_exchange_weak(
                expect,
                make_refcounter(get_refs(expect), color::white),
                std::memory_order_relaxed,
                std::memory_order_relaxed))
          break;
      }
    }

    // Lock wavefront_begin->edges_, for processing.
    std::lock_guard<std::mutex> edges_lck{ wavefront_begin->mtx_ };

    for (const vertex& edge : wavefront_begin->edges_) {
      // Note that if dst has this generation, we short circuit the release
      // manually, to prevent recursion.
      //
      // Note that this does not trip a GC, as only edge changes can do that.
      // And release of this pointer simply releases a control, not its
      // associated edge.
      const intrusive_ptr<base_control> dst = edge.dst_.load();

      // We don't need to lock dst->generation_, since it's this generation
      // which is already protected.
      // (When it isn't this generation, we don't process the edge.)
      if (dst == nullptr || dst->generation_ != this)
        continue;

      // dst color meaning:
      // 1. white -- already processed, skip reprocessing.
      // 2. grey -- either in the wavefront, or marked grey due to red-promotion
      //    (must be moved into wavefront).
      // 3. red -- zero reference count, outside wavefront, requires processing.
      //
      // Only red requires promotion to grey, the other colours remain as is.
      //
      // Note that the current node (*wavefront_begin) is already marked white,
      // thus the handling of that won't mess up anything.
      std::uintptr_t expect = make_refcounter(0, color::red);
      do {
        assert(get_color(expect) != color::black);
      } while (get_color(expect) != color::white
          && dst->store_refs_.compare_exchange_weak(
              expect,
              make_refcounter(get_refs(expect), color::grey),
              std::memory_order_acq_rel,
              std::memory_order_acquire));
      if (get_color(expect) == color::white)
        continue; // Skip already processed element.

      // Ensure grey node is in wavefront.
      // Yes, we may be moving a node already in the wavefront to a different
      // position.
      // Can't be helped, since we're operating outside red-promotion exclusion.
      assert(get_color(expect) != color::black && get_color(expect) != color::white);
      assert(wavefront_begin != controls_.iterator_to(*dst));

      if (wavefront_end == controls_.iterator_to(*dst)) [[unlikely]] {
        ++wavefront_end;
      } else {
        controls_.splice(wavefront_end, controls_, controls_.iterator_to(*dst));
      }
    }

    ++wavefront_begin;
  }

  return wavefront_begin;
}

inline auto generation::gc_phase2_sweep_(controls_list::iterator wavefront_end)
noexcept
-> controls_list::iterator {
  for (controls_list::iterator wavefront_begin = controls_.begin();
      wavefront_begin != wavefront_end;
      ++wavefront_begin) {
    base_control& bc = *wavefront_begin;

    // Change bc colour to white.
    std::uintptr_t expect = make_refcounter(0, color::grey);
    while (get_color(expect) != color::white) {
      assert(get_color(expect) == color::grey);
      if (bc.store_refs_.compare_exchange_weak(
              expect,
              make_refcounter(get_refs(expect), color::white),
              std::memory_order_relaxed,
              std::memory_order_relaxed))
        break;
    }
    if (get_color(expect) == color::white) [[likely]] {
      continue; // Already processed in phase 1.
    }

    // Process edges.
    std::lock_guard<std::mutex> bc_lck{ bc.mtx_ };
    for (const vertex& v : bc.edges_) {
      intrusive_ptr<base_control> dst = v.dst_.get();
      if (dst == nullptr || dst->generation_ != this)
        continue; // Skip edges outside this generation.

      expect = make_refcounter(0, color::red);
      while (get_color(expect) == color::red) {
        if (dst->store_refs_.compare_exchange_weak(
                expect,
                make_refcounter(get_refs(expect), color::grey),
                std::memory_order_relaxed,
                std::memory_order_relaxed))
          break;
      }
      if (get_color(expect) != color::red) [[likely]] {
        continue; // Already processed or already in wavefront.
      }

      assert(dst != &bc);
      assert(wavefront_end != controls_.end());
      assert(wavefront_begin != controls_.iterator_to(*dst));

      if (wavefront_end == controls_.iterator_to(*dst)) [[unlikely]] {
        ++wavefront_end;
      } else {
        controls_.splice(wavefront_end, controls_, controls_.iterator_to(*dst));
      }
    }
  }

  return wavefront_end;
}

inline auto generation::merge_(
    std::tuple<intrusive_ptr<generation>, bool> src_tpl,
    std::tuple<intrusive_ptr<generation>, bool> dst_tpl)
noexcept
-> std::tuple<intrusive_ptr<generation>, bool> {
  // Convenience aliases, must be references because of
  // recursive invocation of this function.
  intrusive_ptr<generation>& src = std::get<0>(src_tpl);
  intrusive_ptr<generation>& dst = std::get<0>(dst_tpl);

  // Arguments assertion.
  assert(src != dst && src != nullptr && dst != nullptr);
  assert(order_invariant(*src, *dst)
      || (src->seq() == dst->seq() && src < dst));

  // Convenience of GC promise booleans.
  bool src_gc_requested = std::get<1>(src_tpl);

  // Lock out GC, controls_ modifications, and merges in src.
  // (We use unique_lock instead of lock_guard, to validate
  // correctness at call to merge0_.)
  const std::unique_lock<std::shared_mutex> src_merge_lck{ src->merge_mtx_ };
  const std::unique_lock<std::shared_mutex> src_lck{ src->mtx_ };

  // Cascade merge operation into edges.
  for (base_control& bc : src->controls_) {
    std::lock_guard<std::mutex> edge_lck{ bc.mtx_ };
    for (const vertex& edge : bc.edges_) {
      // Move edge.
      // We have to restart this, as other threads may change pointers
      // from under us.
      for (auto edge_dst = edge.dst_.load();
          (edge_dst != nullptr
           && edge_dst->generation_ != src
           && edge_dst->generation_ != dst);
          edge_dst = edge.dst_.load()) {
        // Generation check: we only merge if invariant would
        // break after move of ``bc`` into ``dst``.
        const auto edge_dst_gen = edge_dst->generation_.load();
        if (order_invariant(*dst, *edge_dst_gen)) break;

        // Recursion.
        dst_tpl = merge_(
            std::make_tuple(edge_dst_gen, false),
            std::move(dst_tpl));
      }
    }
  }

  // All edges have been moved into or past dst,
  // unless they're in src.
  // Now, we can place src into dst.
  //
  // We update dst_gc_requested.
  std::get<1>(dst_tpl) = merge0_(
      std::make_tuple(src.get(), src_gc_requested),
      std::make_tuple(dst.get(), std::get<1>(dst_tpl)),
      src_lck,
      src_merge_lck);
  return dst_tpl;
}

inline auto generation::merge0_(
    std::tuple<generation*, bool> x,
    std::tuple<generation*, bool> y,
    const std::unique_lock<std::shared_mutex>& x_mtx_lck [[maybe_unused]],
    const std::unique_lock<std::shared_mutex>& x_merge_mtx_lck [[maybe_unused]])
noexcept
-> bool {
  // Convenience of accessing arguments.
  generation* src;
  generation* dst;
  bool src_gc_requested, dst_gc_requested;
  std::tie(src, src_gc_requested, dst, dst_gc_requested) = std::tuple_cat(x, y);
  // Validate ordering.
  assert(src != dst && src != nullptr && dst != nullptr);
  assert(order_invariant(*src, *dst)
      || (src->seq() == dst->seq() && src < dst));
  assert(x_mtx_lck.owns_lock() && x_mtx_lck.mutex() == &src->mtx_);
  assert(x_merge_mtx_lck.owns_lock() && x_merge_mtx_lck.mutex() == &src->merge_mtx_);

  // We promise a GC, because:
  // 1. it's trivial in src
  // 2. the algorithm requires that dst is GC'd at some point
  //    after the merge.
  // Note that, due to ``x_mtx_lck``, no GC can take place on \p src
  // until we're done.
  if (!src_gc_requested) {
    src_gc_requested = !src->gc_flag_.test_and_set();
  } else {
    assert(src->gc_flag_.test_and_set());
  }

  // Propagate responsibility for GC.
  // If another thread has commited to running GC,
  // we'll use that thread's promise,
  // instead of running one ourselves later.
  //
  // This invocation here is an optimization, to prevent
  // other threads from acquiring the responsibility.
  // (Something that'll reduce lock contention probability
  // on ``dst->mtx_``.)
  if (!dst_gc_requested)
    dst_gc_requested = !dst->gc_flag_.test_and_set();
  else
    assert(dst->gc_flag_.test_and_set());

  // Update everything in src, to be moveable to dst.
  // We lock dst now, as our predicates check for dst to be valid.
  assert(x_mtx_lck.owns_lock() && x_mtx_lck.mutex() == &src->mtx_);
  std::lock_guard<std::shared_mutex> dst_lck{ dst->mtx_ };

  // Stage 1: Update edge reference counters.
  for (base_control& bc : src->controls_) {
    std::lock_guard<std::mutex> edge_lck{ bc.mtx_ };
    for (const vertex& edge : bc.edges_) {
      const auto edge_dst = edge.dst_.get();
      assert(edge_dst == nullptr
          || edge_dst->generation_ == src
          || edge_dst->generation_ == dst
          || order_invariant(*dst, *edge_dst->generation_.load()));

      // Update reference counters.
      // (This predicate is why stage 2 must happen after stage 1.)
      if (edge_dst != nullptr && edge_dst->generation_ == dst)
        edge_dst->release(true);
    }
  }
  // Stage 2: switch generation pointers.
  // Note that we can't combine stage 1 and stage 2,
  // as that could cause incorrect detection of cases where
  // release is to be invoked, leading to too many releases.
  for (base_control& bc : src->controls_) {
    assert(bc.generation_ == src);
    bc.generation_ = intrusive_ptr<generation>(dst, true);
  }

  // Splice onto dst.
  dst->controls_.splice(dst->controls_.end(), src->controls_);

  // Fulfill our promise of src GC.
  if (src_gc_requested) {
    // Since src is now empty, GC on it is trivial.
    // So instead of running it, simply clear the flag
    // (but only if we took responsibility for running it).
    src->gc_flag_.clear();
  }

  // Propagate responsibility for GC.
  // If another thread has commited to running GC,
  // we'll use that thread's promise,
  // instead of running one ourselves later.
  //
  // This invocation is repeated here,
  // because if ``!dst_gc_requested``,
  // the thread that promised the GC may have completed.
  // And this could cause missed GC of src elements.
  if (!dst_gc_requested)
    dst_gc_requested = !dst->gc_flag_.test_and_set();
  else
    assert(dst->gc_flag_.test_and_set());

  return dst_gc_requested;
}


inline vertex::vertex()
: vertex(base_control::publisher_lookup(this, sizeof(*this)))
{}

inline vertex::vertex(intrusive_ptr<base_control> bc) noexcept
: bc_(std::move(bc))
{
  assert(bc_ != nullptr);
  bc_->push_back(*this);
}

inline vertex::~vertex() noexcept {
  if (bc_->expired()) {
    assert(dst_ == nullptr);
  } else {
    reset();
  }

  assert(this->link<vertex>::linked());
  bc_->erase(*this);
}

inline auto vertex::reset()
noexcept
-> void {
  if (owner_is_expired()) return; // Reset is a noop when expired.
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

inline auto vertex::reset(
    intrusive_ptr<base_control> new_dst,
    bool has_reference,
    bool no_red_promotion)
noexcept
-> void {
  assert(!has_reference || no_red_promotion);

  if (owner_is_expired()) [[unlikely]] { // Reset is a noop when expired.
    // Clear reference if we hold one.
    if (new_dst != nullptr && has_reference) new_dst->release();
    return;
  }

  // Need to special case this, because below we release ```dst_```.
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
  if (new_dst == nullptr) {
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

inline auto vertex::owner_is_expired() const
noexcept
-> bool {
  return bc_->expired();
}

inline auto vertex::get_control() const
noexcept
-> intrusive_ptr<base_control> {
  return dst_.load();
}


} /* namespace cycle_ptr::detail */


namespace cycle_ptr {


/**
 * \brief Tag indicating an edge without owner.
 * \details
 * Used by cycle_allocator and cycle_member_ptr instances, where there is no
 * origin object participating in the cycle_ptr graph.
 */
struct unowned_cycle_t {};

/**
 * \brief Tag indicating an edge without an owner.
 * \relates unowned_cycle_t
 */
inline constexpr auto unowned_cycle = unowned_cycle_t();

template<typename> class cycle_member_ptr;
template<typename> class cycle_gptr;
template<typename> class cycle_weak_ptr;
template<typename> class cycle_allocator;

template<typename T, typename Alloc, typename... Args>
auto allocate_cycle(Alloc alloc, Args&&... args) -> cycle_gptr<T>;

/**
 * \brief An optional base for classes which need to supply ownership to cycle_member_ptr.
 * \details
 * The cycle_base keeps track of the control block of the object participating
 * in the cycle_ptr graph, as well as providing a *shared from this* utility.
 *
 * You are not required to inherit from cycle_base, for the cycle_ptr graph to
 * function correctly.
 */
class cycle_base {
  template<typename> friend class cycle_member_ptr;
  template<typename> friend class cycle_allocator;

 protected:
  /**
   * \brief Default constructor acquires its control block from context.
   * \details
   * Uses publisher logic to look up the control block for its range.
   * Those ranges are published by cycle_allocator, make_cycle, and allocate_cycle.
   *
   * \throws std::runtime_error if no range was published.
   */
  cycle_base()
  : control_(detail::base_control::publisher_lookup(this, sizeof(*this)))
  {}

  /**
   * \brief Specialized constructor that signifies ``*this`` will not be pointed at by a cycle_ptr.
   * \throws std::bad_alloc If there is not enough memory to create
   * the required control block.
   */
  cycle_base(unowned_cycle_t unowned_tag [[maybe_unused]])
  : control_(detail::base_control::unowned_control())
  {}

  /**
   * \brief Copy constructor.
   * \details
   * Provided so that you don't lose the default copy constructor semantics,
   * but keep in mind that this constructor simply invokes the default constructor.
   *
   * \note A copy has a different, automatically deduced, control block.
   *
   * \throws std::runtime_error if no range was published.
   */
  cycle_base([[maybe_unused]] const cycle_base&)
  : cycle_base()
  {}

  /**
   * \brief Move constructor.
   * \details
   * Provided so that you don't lose the default move constructor semantics,
   * but keep in mind that this constructor simply invokes the default constructor.
   *
   * \note A copy has a different, automatically deduced, control block.
   *
   * \throws std::runtime_error if no range was published.
   */
  cycle_base([[maybe_unused]] cycle_base&&)
  : cycle_base()
  {}

  /**
   * \brief Copy assignment.
   * \details
   * A noop, provided so you don't lose default assignment in derived classes.
   */
  auto operator=([[maybe_unused]] const cycle_base&)
  noexcept
  -> cycle_base& {
    return *this;
  }

  /**
   * \brief Move assignment.
   * \details
   * A noop, provided so you don't lose default assignment in derived classes.
   */
  auto operator=([[maybe_unused]] cycle_base&&)
  noexcept
  -> cycle_base& {
    return *this;
  }

  ///\brief Default destructor.
  ~cycle_base() noexcept = default;

  /**
   * \brief Create a cycle_gptr (equivalent of std::shared_ptr) from this.
   * \details
   * The returned smart pointer uses the control block of this.
   *
   * Instead of using ``this->shared_from_this(this)``, you could use this
   * to create pointers directly from member variables, by invocing
   * ``this->shared_from_this(&this->member_variable)``.
   *
   * \throws std::bad_weak_ptr If the smart pointer can not be created.
   * This occurs when invoked during constructor of derived type, or during its destruction.
   * Note that if unowned_cycle is used to construct the base, this method will
   * always throw ``std::bad_weak_ptr``.
   */
  template<typename T>
  auto shared_from_this(T* this_ptr) const
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
  ///\brief Pointer to control block.
  const detail::intrusive_ptr<detail::base_control> control_;
};

/**
 * \brief Pointer between objects participating in the cycle_ptr graph.
 * \details
 * This smart pointer models the relationship between an origin object
 * and a target object.
 *
 * It is intended for use in member variables, as well as collections
 * that are owned by a member variable.
 */
template<typename T>
class cycle_member_ptr
: private detail::vertex
{
  template<typename> friend class cycle_member_ptr;
  template<typename> friend class cycle_gptr;
  template<typename> friend class cycle_weak_ptr;

 public:
  ///\brief Element type of this pointer.
  using element_type = std::remove_extent_t<T>;
  ///\brief Weak pointer equivalent.
  using weak_type = cycle_weak_ptr<T>;

  /**
   * \brief Create an unowned member pointer, representing a nullptr.
   * \details
   * Makes this cycle_member_ptr behave like a cycle_gptr.
   *
   * Useful for instance when you need a cycle_member_ptr to pass as argument
   * to a std::map::find() function, for instance:
   * \code
   * std::map<
   *     cycle_member_ptr<Key>, Value,
   *     std::less<cycle_member_ptr<Key>>,
   *     cycle_allocator<std::allocator<std::pair<const cycle_member_ptr<Key>, Value>>>>
   *     someMap;
   * cycle_gptr<Key> soughtKey;
   *
   * // This will fail, as conversion from cycle_gptr to cycle_member_ptr
   * // without declared ownership uses automatic ownership, but none is
   * // published.
   * // It will fail with std::runtime_error.
   * someMap.find(soughtKey);
   *
   * // This will succeed, as conversion now explicitly declares
   * // the argument to std::map::find() to have the correct type.
   * someMap.find(cycle_member_ptr<Key>(unowned_cycle, soughtKey));
   *
   * // Braces initalization should also work:
   * someMap.find({unowned_cycle, soughtKey});
   * \endcode
   *
   * \post
   * *this == nullptr
   *
   * \param unowned_tag Tag to select ownerless construction.
   */
  explicit cycle_member_ptr(unowned_cycle_t unowned_tag [[maybe_unused]]) noexcept
  : vertex(detail::base_control::unowned_control())
  {}

  /**
   * \brief Create an unowned member pointer, representing a nullptr.
   * \details
   * Makes this cycle_member_ptr behave like a cycle_gptr.
   *
   * Useful for instance when you need a cycle_member_ptr to pass as argument
   * to a std::map::find() function, for instance:
   * \code
   * std::map<
   *     cycle_member_ptr<Key>, Value,
   *     std::less<cycle_member_ptr<Key>>,
   *     cycle_allocator<std::allocator<std::pair<const cycle_member_ptr<Key>, Value>>>>
   *     someMap;
   * cycle_gptr<Key> soughtKey;
   *
   * // This will fail, as conversion from cycle_gptr to cycle_member_ptr
   * // without declared ownership uses automatic ownership, but none is
   * // published.
   * // It will fail with std::runtime_error.
   * someMap.find(soughtKey);
   *
   * // This will succeed, as conversion now explicitly declares
   * // the argument to std::map::find() to have the correct type.
   * someMap.find(cycle_member_ptr<Key>(unowned_cycle, soughtKey));
   *
   * // Braces initalization should also work:
   * someMap.find({unowned_cycle, soughtKey});
   * \endcode
   *
   * \post
   * *this == nullptr
   *
   * \param unowned_tag Tag to select ownerless construction.
   * \param nil Nullptr value.
   */
  cycle_member_ptr(unowned_cycle_t unowned_tag, std::nullptr_t nil [[maybe_unused]]) noexcept
  : cycle_member_ptr(unowned_tag)
  {}

  /**
   * \brief Create an unowned member pointer, pointing at \p ptr.
   * \details
   * Makes this cycle_member_ptr behave like a cycle_gptr.
   *
   * Useful for instance when you need a cycle_member_ptr to pass as argument
   * to a std::map::find() function, for instance:
   * \code
   * std::map<
   *     cycle_member_ptr<Key>, Value,
   *     std::less<cycle_member_ptr<Key>>,
   *     cycle_allocator<std::allocator<std::pair<const cycle_member_ptr<Key>, Value>>>>
   *     someMap;
   * cycle_gptr<Key> soughtKey;
   *
   * // This will fail, as conversion from cycle_gptr to cycle_member_ptr
   * // without declared ownership uses automatic ownership, but none is
   * // published.
   * // It will fail with std::runtime_error.
   * someMap.find(soughtKey);
   *
   * // This will succeed, as conversion now explicitly declares
   * // the argument to std::map::find() to have the correct type.
   * someMap.find(cycle_member_ptr<Key>(unowned_cycle, soughtKey));
   *
   * // Braces initalization should also work:
   * someMap.find({unowned_cycle, soughtKey});
   * \endcode
   *
   * \post
   * *this == ptr
   *
   * \param unowned_tag Tag to select ownerless construction.
   * \param ptr Initialize to point at this same object.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(unowned_cycle_t unowned_tag [[maybe_unused]], const cycle_member_ptr<U>& ptr) noexcept
  : detail::vertex(detail::base_control::unowned_control()),
    target_(ptr.target_)
  {
    if (ptr.owner_is_expired()) {
      target_ = nullptr;
    } else {
      this->detail::vertex::reset(ptr.get_control(), false, false);
    }
  }

  /**
   * \brief Create an unowned member pointer, pointing at \p ptr.
   * \details
   * Makes this cycle_member_ptr behave like a cycle_gptr.
   *
   * Useful for instance when you need a cycle_member_ptr to pass as argument
   * to a std::map::find() function, for instance:
   * \code
   * std::map<
   *     cycle_member_ptr<Key>, Value,
   *     std::less<cycle_member_ptr<Key>>,
   *     cycle_allocator<std::allocator<std::pair<const cycle_member_ptr<Key>, Value>>>>
   *     someMap;
   * cycle_gptr<Key> soughtKey;
   *
   * // This will fail, as conversion from cycle_gptr to cycle_member_ptr
   * // without declared ownership uses automatic ownership, but none is
   * // published.
   * // It will fail with std::runtime_error.
   * someMap.find(soughtKey);
   *
   * // This will succeed, as conversion now explicitly declares
   * // the argument to std::map::find() to have the correct type.
   * someMap.find(cycle_member_ptr<Key>(unowned_cycle, soughtKey));
   *
   * // Braces initalization should also work:
   * someMap.find({unowned_cycle, soughtKey});
   * \endcode
   *
   * \post
   * *this == original value of ptr
   *
   * \post
   * ptr == nullptr
   *
   * \param unowned_tag Tag to select ownerless construction.
   * \param ptr Initialize to point at this same object.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(unowned_cycle_t unowned_tag, cycle_member_ptr<U>&& ptr) noexcept
  : cycle_member_ptr(unowned_tag, ptr)
  {
    ptr.reset();
  }

  /**
   * \brief Create an unowned member pointer, pointing at \p ptr.
   * \details
   * Makes this cycle_member_ptr behave like a cycle_gptr.
   *
   * Useful for instance when you need a cycle_member_ptr to pass as argument
   * to a std::map::find() function, for instance:
   * \code
   * std::map<
   *     cycle_member_ptr<Key>, Value,
   *     std::less<cycle_member_ptr<Key>>,
   *     cycle_allocator<std::allocator<std::pair<const cycle_member_ptr<Key>, Value>>>>
   *     someMap;
   * cycle_gptr<Key> soughtKey;
   *
   * // This will fail, as conversion from cycle_gptr to cycle_member_ptr
   * // without declared ownership uses automatic ownership, but none is
   * // published.
   * // It will fail with std::runtime_error.
   * someMap.find(soughtKey);
   *
   * // This will succeed, as conversion now explicitly declares
   * // the argument to std::map::find() to have the correct type.
   * someMap.find(cycle_member_ptr<Key>(unowned_cycle, soughtKey));
   *
   * // Braces initalization should also work:
   * someMap.find({unowned_cycle, soughtKey});
   * \endcode
   *
   * \post
   * *this == ptr
   *
   * \param unowned_tag Tag to select ownerless construction.
   * \param ptr Initialize to point at this same object.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(unowned_cycle_t unowned_tag [[maybe_unused]], const cycle_gptr<U>& ptr) noexcept
  : detail::vertex(detail::base_control::unowned_control()),
    target_(ptr.target_)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  /**
   * \brief Create an unowned member pointer, pointing at \p ptr.
   * \details
   * Makes this cycle_member_ptr behave like a cycle_gptr.
   *
   * Useful for instance when you need a cycle_member_ptr to pass as argument
   * to a std::map::find() function, for instance:
   * \code
   * std::map<
   *     cycle_member_ptr<Key>, Value,
   *     std::less<cycle_member_ptr<Key>>,
   *     cycle_allocator<std::allocator<std::pair<const cycle_member_ptr<Key>, Value>>>>
   *     someMap;
   * cycle_gptr<Key> soughtKey;
   *
   * // This will fail, as conversion from cycle_gptr to cycle_member_ptr
   * // without declared ownership uses automatic ownership, but none is
   * // published.
   * // It will fail with std::runtime_error.
   * someMap.find(soughtKey);
   *
   * // This will succeed, as conversion now explicitly declares
   * // the argument to std::map::find() to have the correct type.
   * someMap.find(cycle_member_ptr<Key>(unowned_cycle, soughtKey));
   *
   * // Braces initalization should also work:
   * someMap.find({unowned_cycle, soughtKey});
   * \endcode
   *
   * \post
   * *this == original value of ptr
   *
   * \post
   * ptr == nullptr
   *
   * \param unowned_tag Tag to select ownerless construction.
   * \param ptr Initialize to point at this same object.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(unowned_cycle_t unowned_tag [[maybe_unused]], cycle_gptr<U>&& ptr) noexcept
  : detail::vertex(detail::base_control::unowned_control()),
    target_(std::exchange(ptr.target_, nullptr))
  {
    this->detail::vertex::reset(
        std::move(ptr.target_ctrl_),
        true, true);
  }

  /**
   * \brief Aliasing constructor for unowned member pointer.
   * \details
   * Makes this cycle_member_ptr behave like a cycle_gptr.
   *
   * \post
   * control block of *this == control block of ptr
   *
   * \post
   * this->get() == target
   *
   * \bug It looks like std::shared_ptr allows similar aliases to be
   * constructed without \p ptr having ownership of anything, yet still
   * pointing at \p target.
   * This case is currently unspecified in cycle_ptr library.
   */
  template<typename U>
  cycle_member_ptr(unowned_cycle_t unowned_tag [[maybe_unused]], const cycle_member_ptr<U>& ptr, element_type* target) noexcept
  : detail::vertex(detail::base_control::unowned_control()),
    target_(target)
  {
    if (ptr.owner_is_expired()) {
      target_ = nullptr;
    } else {
      this->detail::vertex::reset(ptr.get_control(), false, false);
    }
  }

  /**
   * \brief Aliasing constructor for unowned member pointer.
   * \details
   * Makes this cycle_member_ptr behave like a cycle_gptr.
   *
   * \post
   * control block of *this == control block of ptr
   *
   * \post
   * this->get() == target
   *
   * \bug It looks like std::shared_ptr allows similar aliases to be
   * constructed without \p ptr having ownership of anything, yet still
   * pointing at \p target.
   * This case is currently unspecified in cycle_ptr library.
   */
  template<typename U>
  cycle_member_ptr(unowned_cycle_t unowned_tag [[maybe_unused]], const cycle_gptr<U>& ptr, element_type* target)
  : detail::vertex(detail::base_control::unowned_control()),
    target_(target)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  /**
   * \brief Create an unowned member pointer, pointing at \p ptr.
   * \details
   * Makes this cycle_member_ptr behave like a cycle_gptr.
   *
   * Useful for instance when you need a cycle_member_ptr to pass as argument
   * to a std::map::find() function, for instance:
   * \code
   * std::map<
   *     cycle_member_ptr<Key>, Value,
   *     std::less<cycle_member_ptr<Key>>,
   *     cycle_allocator<std::allocator<std::pair<const cycle_member_ptr<Key>, Value>>>>
   *     someMap;
   * cycle_gptr<Key> soughtKey;
   *
   * // This will fail, as conversion from cycle_gptr to cycle_member_ptr
   * // without declared ownership uses automatic ownership, but none is
   * // published.
   * // It will fail with std::runtime_error.
   * someMap.find(soughtKey);
   *
   * // This will succeed, as conversion now explicitly declares
   * // the argument to std::map::find() to have the correct type.
   * someMap.find(cycle_member_ptr<Key>(unowned_cycle, soughtKey));
   *
   * // Braces initalization should also work:
   * someMap.find({unowned_cycle, soughtKey});
   * \endcode
   *
   * \post
   * *this == ptr.lock()
   *
   * \param unowned_tag Tag to select ownerless construction.
   * \param ptr Initialize to point at this same object.
   * \throws std::bad_weak_ptr If the weak pointer is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(unowned_cycle_t unowned_tag, const cycle_weak_ptr<U>& ptr)
  : cycle_member_ptr(unowned_tag, cycle_gptr<U>(ptr))
  {}

  /**
   * \brief Constructor with explicitly specified ownership.
   * \details
   * Creates a pointer owned by this owner.
   *
   * \post
   * *this == nullptr
   *
   * \param owner The owner object of this member pointer.
   */
  explicit cycle_member_ptr(cycle_base& owner) noexcept
  : vertex(owner.control_)
  {}

  /**
   * \brief Constructor with explicitly specified ownership.
   * \details
   * Creates a pointer owned by this owner.
   *
   * \post
   * *this == nullptr
   *
   * \param owner The owner object of this member pointer.
   * \param nil ``nullptr``
   */
  cycle_member_ptr(cycle_base& owner, std::nullptr_t nil [[maybe_unused]]) noexcept
  : cycle_member_ptr(owner)
  {}

  /**
   * \brief Constructor with explicitly specified ownership.
   * \details
   * Creates a pointer owned by this owner.
   *
   * \post
   * *this == ptr
   *
   * \param owner The owner object of this member pointer.
   * \param ptr Pointer to initalize with.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, const cycle_member_ptr<U>& ptr)
  : detail::vertex(owner.control_),
    target_(ptr.target_)
  {
    if (ptr.owner_is_expired()) {
      target_ = nullptr;
    } else {
      this->detail::vertex::reset(ptr.get_control(), false, false);
    }
  }

  /**
   * \brief Constructor with explicitly specified ownership.
   * \details
   * Creates a pointer owned by this owner.
   *
   * \post
   * *this == original value of ptr
   *
   * \post
   * ptr == nullptr
   *
   * \param owner The owner object of this member pointer.
   * \param ptr Pointer to initalize with.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, cycle_member_ptr<U>&& ptr)
  : cycle_member_ptr(owner, ptr)
  {
    ptr.reset();
  }

  /**
   * \brief Constructor with explicitly specified ownership.
   * \details
   * Creates a pointer owned by this owner.
   *
   * \post
   * *this == ptr
   *
   * \param owner The owner object of this member pointer.
   * \param ptr Pointer to initalize with.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, const cycle_gptr<U>& ptr)
  : detail::vertex(owner.control_),
    target_(ptr.target_)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  /**
   * \brief Constructor with explicitly specified ownership.
   * \details
   * Creates a pointer owned by this owner.
   *
   * \post
   * *this == original value of ptr
   *
   * \post
   * ptr == nullptr
   *
   * \param owner The owner object of this member pointer.
   * \param ptr Pointer to initalize with.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, cycle_gptr<U>&& ptr)
  : detail::vertex(owner.control_),
    target_(std::exchange(ptr.target_, nullptr))
  {
    this->detail::vertex::reset(
        std::move(ptr.target_ctrl_),
        true, true);
  }

  /**
   * \brief Aliasing constructor with explicitly specified ownership.
   * \details
   * Creates a pointer owned by this owner.
   *
   * \post
   * control block of *this == control block of ptr
   *
   * \post
   * this->get() == target
   *
   * \param owner The owner object of this member pointer.
   * \param ptr Pointer to initalize with.
   * \param target The alias pointer.
   *
   * \bug It looks like std::shared_ptr allows similar aliases to be
   * constructed without \p ptr having ownership of anything, yet still
   * pointing at \p target.
   * This case is currently unspecified in cycle_ptr library.
   */
  template<typename U>
  cycle_member_ptr(cycle_base& owner, const cycle_member_ptr<U>& ptr, element_type* target)
  : detail::vertex(owner.control_),
    target_(target)
  {
    if (ptr.owner_is_expired()) {
      target_ = nullptr;
    } else {
      this->detail::vertex::reset(ptr.get_control(), false, false);
    }
  }

  /**
   * \brief Aliasing constructor with explicitly specified ownership.
   * \details
   * Creates a pointer owned by this owner.
   *
   * \post
   * control block of *this == control block of ptr
   *
   * \post
   * this->get() == target
   *
   * \param owner The owner object of this member pointer.
   * \param ptr Pointer to initalize with.
   * \param target The alias pointer.
   *
   * \bug It looks like std::shared_ptr allows similar aliases to be
   * constructed without \p ptr having ownership of anything, yet still
   * pointing at \p target.
   * This case is currently unspecified in cycle_ptr library.
   */
  template<typename U>
  cycle_member_ptr(cycle_base& owner, const cycle_gptr<U>& ptr, element_type* target)
  : detail::vertex(owner.control_),
    target_(target)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  /**
   * \brief Constructor with explicitly specified ownership.
   *
   * \post
   * *this == ptr.lock()
   *
   * \param owner The owner object of this member pointer.
   * \param ptr Initialize to point at this same object.
   * \throws std::bad_weak_ptr If the weak pointer is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_base& owner, const cycle_weak_ptr<U>& ptr)
  : cycle_member_ptr(owner, cycle_gptr<U>(ptr))
  {}

  /**
   * \brief Constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * *this == nullptr
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  cycle_member_ptr() {}

  /**
   * \brief Constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * *this == nullptr
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  cycle_member_ptr(std::nullptr_t nil [[maybe_unused]]) {}

  /**
   * \brief Constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * *this == ptr
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  cycle_member_ptr(const cycle_member_ptr& ptr)
  : target_(ptr.target_)
  {
    if (ptr.owner_is_expired()) {
      target_ = nullptr;
    } else {
      this->detail::vertex::reset(ptr.get_control(), false, false);
    }
  }

  /**
   * \brief Constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * *this == original value of ptr
   *
   * \post
   * ptr == nullptr
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  cycle_member_ptr(cycle_member_ptr&& ptr)
  : cycle_member_ptr(ptr)
  {
    ptr.reset();
  }

  /**
   * \brief Constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * *this == ptr
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(const cycle_member_ptr<U>& ptr)
  : target_(ptr.target_)
  {
    if (ptr.owner_is_expired()) {
      target_ = nullptr;
    } else {
      this->detail::vertex::reset(ptr.get_control(), false, false);
    }
  }

  /**
   * \brief Constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * *this == original value of ptr
   *
   * \post
   * ptr == nullptr
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_member_ptr<U>&& ptr)
  : cycle_member_ptr(ptr)
  {
    ptr.reset();
  }

  /**
   * \brief Constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * *this == ptr
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(const cycle_gptr<U>& ptr)
  : target_(ptr.target_)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  /**
   * \brief Constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * *this == original value of ptr
   *
   * \post
   * ptr == nullptr
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_member_ptr(cycle_gptr<U>&& ptr)
  : target_(std::exchange(ptr.target_, nullptr))
  {
    this->detail::vertex::reset(
        std::move(ptr.target_ctrl_),
        true, true);
  }

  /**
   * \brief Aliasing constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * control block of *this == control block of ptr
   *
   * \post
   * this->get() == target
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  template<typename U>
  cycle_member_ptr(const cycle_member_ptr<U>& ptr, element_type* target)
  : target_(target)
  {
    if (ptr.owner_is_expired()) {
      target_ = nullptr;
    } else {
      this->detail::vertex::reset(ptr.get_control(), false, false);
    }
  }

  /**
   * \brief Aliasing constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * control block of *this == control block of ptr
   *
   * \post
   * this->get() == target
   *
   * \throws std::runtime_error If no published control block covers
   * the address range of *this.
   */
  template<typename U>
  cycle_member_ptr(const cycle_gptr<U>& ptr, element_type* target)
  : target_(target)
  {
    this->detail::vertex::reset(ptr.target_ctrl_, false, true);
  }

  /**
   * \brief Constructor with automatic ownership detection.
   * \details
   * Uses \ref cycle_ptr::detail::base_control::publisher "publisher" logic
   * to figure out the ownership.
   *
   * \ref cycle_ptr::allocate_cycle,
   * \ref cycle_ptr::make_cycle,
   * and \ref cycle_ptr::cycle_allocator
   * publish their control block, which is automatically picked up by this
   * contructor.
   *
   * \post
   * *this == ptr.lock()
   *
   * \param ptr Initialize to point at this same object.
   * \throws std::bad_weak_ptr If the weak pointer is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  explicit cycle_member_ptr(const cycle_weak_ptr<U>& ptr)
  : cycle_member_ptr(cycle_gptr<U>(ptr))
  {}

  /**
   * \brief Assignment operator.
   * \post
   * *this == nullptr
   *
   * \throws std::runtime_error if the owner of this is expired.
   */
  auto operator=(std::nullptr_t nil [[maybe_unused]])
  noexcept
  -> cycle_member_ptr& {
    reset();
    return *this;
  }

  /**
   * \brief Copy assignment operator.
   * \post
   * *this == other
   *
   * \throws std::runtime_error if the owner of this is expired.
   * \throws std::runtime_error if the owner of other is expired.
   */
  auto operator=(const cycle_member_ptr& other)
  noexcept
  -> cycle_member_ptr& {
    if (other.owner_is_expired()) {
      reset();
    } else {
      this->detail::vertex::reset(other.get_control(), false, false);
      target_ = other.target_;
    }
    return *this;
  }

  /**
   * \brief Move assignment operator.
   * \post
   * *this == original value of other
   *
   * \post
   * other == nullptr
   *
   * \throws std::runtime_error if the owner of this is expired.
   * \throws std::runtime_error if the owner of other is expired.
   */
  auto operator=(cycle_member_ptr&& other)
  noexcept
  -> cycle_member_ptr& {
    if (this != &other) [[likely]] {
      *this = other;
      other.reset();
    }
    return *this;
  }

  /**
   * \brief Copy assignment operator.
   * \post
   * *this == other
   *
   * \throws std::runtime_error if the owner of this is expired.
   * \throws std::runtime_error if the owner of other is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_member_ptr<U>& other)
  noexcept
  -> cycle_member_ptr& {
    if (other.owner_is_expired()) {
      reset();
    } else {
      this->detail::vertex::reset(other.get_control(), false, false);
      target_ = other.target_;
    }
    return *this;
  }

  /**
   * \brief Move assignment operator.
   * \post
   * *this == original value of other
   *
   * \post
   * other == nullptr
   *
   * \throws std::runtime_error if the owner of this is expired.
   * \throws std::runtime_error if the owner of other is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(cycle_member_ptr<U>&& other)
  noexcept
  -> cycle_member_ptr& {
    *this = other;
    other.reset();
    return *this;
  }

  /**
   * \brief Copy assignment operator.
   * \post
   * *this == other
   *
   * \throws std::runtime_error if the owner of this is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_gptr<U>& other)
  noexcept
  -> cycle_member_ptr& {
    this->detail::vertex::reset(other.target_ctrl_, false, true);
    target_ = other.target_;
    return *this;
  }

  /**
   * \brief Move assignment operator.
   * \post
   * *this == original value of other
   *
   * \post
   * other == nullptr
   *
   * \throws std::runtime_error if the owner of this is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(cycle_gptr<U>&& other)
  noexcept
  -> cycle_member_ptr& {
    this->detail::vertex::reset(
        std::move(other.target_ctrl_),
        true, true);
    target_ = std::exchange(other.target_, nullptr);
    return *this;
  }

  /**
   * \brief Clear this pointer.
   * \post
   * *this == nullptr
   *
   * \throws std::runtime_error if the owner of this is expired.
   */
  auto reset()
  noexcept
  -> void {
    this->detail::vertex::reset();
    target_ = nullptr;
  }

  /**
   * \brief Swap with other pointer.
   * \post
   * *this == original value of other
   * other == original value of *this
   *
   * \throws std::runtime_error if the owner of this is expired.
   * \throws std::runtime_error if the owner of other is expired.
   */
  auto swap(cycle_member_ptr& other)
  noexcept
  -> void {
    std::tie(*this, other) = std::forward_as_tuple(
        cycle_gptr<T>(std::move(other)),
        cycle_gptr<T>(std::move(*this)));
  }

  /**
   * \brief Swap with other pointer.
   * \post
   * *this == original value of other
   * other == original value of *this
   *
   * \throws std::runtime_error if the owner of this is expired.
   * \throws std::runtime_error if the owner of other is expired.
   */
  auto swap(cycle_gptr<T>& other)
  noexcept
  -> void {
    std::tie(*this, other) = std::forward_as_tuple(
        cycle_gptr<T>(std::move(other)),
        cycle_gptr<T>(std::move(*this)));
  }

  /**
   * \brief Returns the raw pointer of this.
   * \throws std::runtime_error if the owner of this is expired.
   */
  auto get() const
  noexcept
  -> T* {
    if (owner_is_expired()) [[unlikely]]
      return nullptr;
    return target_;
  }

  /**
   * \brief Dereference operation.
   * \details
   * Only declared if \p T is not ``void``.
   * \throws std::runtime_error if the owner of this is expired.
   */
  template<bool Enable = !std::is_void_v<T>>
  auto operator*() const
  noexcept
  -> std::enable_if_t<Enable, T>& {
    assert(get() != nullptr);
    return *get();
  }

  /**
   * \brief Indirection operation.
   * \details
   * Only declared if \p T is not ``void``.
   * \throws std::runtime_error if the owner of this is expired.
   */
  template<bool Enable = !std::is_void_v<T>>
  auto operator->() const
  noexcept
  -> std::enable_if_t<Enable, T>* {
    assert(get() != nullptr);
    return get();
  }

  /**
   * \brief Test if this pointer points holds a non-nullptr value.
   * \returns get() != nullptr
   * \throws std::runtime_error if the owner of this is expired.
   */
  explicit operator bool() const
  noexcept {
    return get() != nullptr;
  }

  ///\brief Ownership ordering.
  template<typename U>
  auto owner_before(const cycle_weak_ptr<U>& other) const
  noexcept
  -> bool {
    return get_control() < other.target_ctrl_;
  }

  ///\brief Ownership ordering.
  template<typename U>
  auto owner_before(const cycle_gptr<U>& other) const
  noexcept
  -> bool {
    return get_control() < other.target_ctrl_;
  }

  ///\brief Ownership ordering.
  template<typename U>
  auto owner_before(const cycle_member_ptr<U>& other) const
  noexcept
  -> bool {
    return get_control() < other.get_control();
  }

 private:
  ///\brief Target object that this points at.
  T* target_ = nullptr;
};

/**
 * \brief Global (or automatic) scope smart pointer.
 * \details
 * This smart pointer models a reference to a target object,
 * from a globally reachable place, such as a function variable.
 *
 * Use this pointer in function arguments/body, global scope, or objects not
 * participating in the cycle_ptr graph.
 *
 * It is smaller and faster than cycle_member_ptr.
 */
template<typename T>
class cycle_gptr {
  template<typename> friend class cycle_member_ptr;
  template<typename> friend class cycle_gptr;
  template<typename> friend class cycle_weak_ptr;
  friend class cycle_base;

  template<typename Type, typename Alloc, typename... Args>
  friend auto cycle_ptr::allocate_cycle(Alloc alloc, Args&&... args) -> cycle_gptr<Type>;

 public:
  ///\copydoc cycle_member_ptr::element_type
  using element_type = std::remove_extent_t<T>;
  ///\copydoc cycle_member_ptr::weak_type
  using weak_type = cycle_weak_ptr<T>;

  ///\brief Default constructor.
  ///\post *this == nullptr
  constexpr cycle_gptr() noexcept {}

  ///\brief Nullptr constructor.
  ///\post *this == nullptr
  constexpr cycle_gptr(std::nullptr_t nil [[maybe_unused]]) noexcept
  : cycle_gptr()
  {}

  /**
   * \brief Copy constructor.
   * \post
   * *this == other
   */
  cycle_gptr(const cycle_gptr& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {
    if (target_ctrl_ != nullptr) target_ctrl_->acquire_no_red();
  }

  /**
   * \brief Move constructor.
   * \post
   * *this == original value of other
   *
   * \post
   * other == nullptr
   */
  cycle_gptr(cycle_gptr&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  /**
   * \brief Copy constructor.
   * \post
   * *this == other
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_gptr(const cycle_gptr<U>& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {
    if (target_ctrl_ != nullptr) target_ctrl_->acquire_no_red();
  }

  /**
   * \brief Move constructor.
   * \post
   * *this == original value of other
   *
   * \post
   * other == nullptr
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_gptr(cycle_gptr<U>&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  /**
   * \brief Copy constructor.
   * \post
   * *this == other
   *
   * \throws std::runtime_error if owner of \p other is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_gptr(const cycle_member_ptr<U>& other)
  : target_(other.target_),
    target_ctrl_(other.get_control())
  {
    if (other.owner_is_expired()) {
      target_ = nullptr;
      target_ctrl_.reset();
    } else if (target_ctrl_ != nullptr) {
      target_ctrl_->acquire();
    }
  }

  /**
   * \brief Move constructor.
   * \post
   * *this == original value of other
   *
   * \post
   * other == nullptr
   *
   * \throws std::runtime_error if owner of \p other is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_gptr(cycle_member_ptr<U>&& other)
  : cycle_gptr(other)
  {
    other.reset();
  }

  /**
   * \brief Aliasing constructor.
   * \post
   * control block of this == control block of other
   *
   * \post
   * this->get() == target
   */
  template<typename U>
  cycle_gptr(const cycle_gptr<U>& other, element_type* target) noexcept
  : target_(target),
    target_ctrl_(other.target_ctrl_)
  {
    if (target_ctrl_ != nullptr) target_ctrl_->acquire_no_red();
  }

  /**
   * \brief Aliasing constructor.
   * \post
   * control block of this == control block of other
   *
   * \post
   * this->get() == target
   *
   * \throws std::runtime_error if owner of \p other is expired.
   */
  template<typename U>
  cycle_gptr(const cycle_member_ptr<U>& other, element_type* target)
  : target_(target),
    target_ctrl_(other.get_control())
  {
    if (other.owner_is_expired()) {
      target_ = nullptr;
      target_ctrl_.reset();
    } else {
      if (target_ctrl_ != nullptr) target_ctrl_->acquire();
    }
  }

  /**
   * \brief Construct from cycle_weak_ptr.
   * \post
   * *this == other.lock()
   *
   * \throws std::bad_weak_ptr if other is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  explicit cycle_gptr(const cycle_weak_ptr<U>& other)
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {
    if (target_ctrl_ == nullptr || !target_ctrl_->weak_acquire())
      throw std::bad_weak_ptr();
  }

  /**
   * \brief Copy assignment.
   * \post
   * *this == other
   */
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

  /**
   * \brief Move assignment.
   * \post
   * *this == original value of other
   *
   * \post
   * other == nullptr
   */
  auto operator=(cycle_gptr&& other)
  noexcept
  -> cycle_gptr& {
    auto bc = std::move(other.target_ctrl_);

    target_ = std::exchange(other.target_, nullptr);
    bc.swap(target_ctrl_);
    if (bc != nullptr) bc->release(bc == target_ctrl_);

    return *this;
  }

  /**
   * \brief Copy assignment.
   * \post
   * *this == other
   */
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

  /**
   * \brief Move assignment.
   * \post
   * *this == original value of other
   *
   * \post
   * other == nullptr
   */
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

  /**
   * \brief Copy assignment.
   * \post
   * *this == other
   *
   * \throws std::runtime_error if other is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_member_ptr<U>& other)
  -> cycle_gptr& {
    if (other.owner_is_expired()) {
      reset();
    } else {
      detail::intrusive_ptr<detail::base_control> bc = other.get_control();
      if (bc != nullptr) bc->acquire();

      target_ = other.target_;
      bc.swap(target_ctrl_);
      if (bc != nullptr) bc->release(bc == target_ctrl_);
    }

    return *this;
  }

  /**
   * \brief Move assignment.
   * \post
   * *this == original value of other
   *
   * \post
   * other == nullptr
   *
   * \throws std::runtime_error if other is expired.
   */
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

  /**
   * \brief Clear this pointer.
   * \post
   * *this == nullptr
   */
  auto reset()
  noexcept
  -> void {
    if (target_ctrl_ != nullptr) {
      target_ = nullptr;
      target_ctrl_->release();
      target_ctrl_.reset();
    }
  }

  /**
   * \brief Swap with \p other.
   * \post
   * *this == original value of other
   *
   * \post
   * other == original value of *this
   */
  auto swap(cycle_gptr& other)
  noexcept
  -> void {
    std::swap(target_, other.target_);
    target_ctrl_.swap(other.target_ctrl_);
  }

  /**
   * \brief Swap with \p other.
   * \post
   * *this == original value of other
   *
   * \post
   * other == original value of *this
   *
   * \throws std::runtime_error if other is expired.
   */
  auto swap(cycle_member_ptr<T>& other)
  -> void {
    other.swap(*this);
  }

  /**
   * \brief Retrieve address of this pointer.
   */
  auto get() const
  noexcept
  -> T* {
    return target_;
  }

  /**
   * \brief Dereference operation.
   * \attention If ``*this == nullptr``, behaviour is undefined.
   */
  template<bool Enable = !std::is_void_v<T>>
  auto operator*() const
  -> std::enable_if_t<Enable, T>& {
    assert(get() != nullptr);
    return *get();
  }

  /**
   * \brief Indirection operation.
   * \attention If ``*this == nullptr``, behaviour is undefined.
   */
  template<bool Enable = !std::is_void_v<T>>
  auto operator->() const
  -> std::enable_if_t<Enable, T>* {
    assert(get() != nullptr);
    return get();
  }

  /**
   * \brief Test if this holds a non-nullptr.
   * \returns ``get() != nullptr``.
   */
  explicit operator bool() const noexcept {
    return get() != nullptr;
  }

  ///\copydoc cycle_member_ptr::owner_before
  template<typename U>
  auto owner_before(const cycle_weak_ptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.target_ctrl_;
  }

  ///\copydoc cycle_member_ptr::owner_before
  template<typename U>
  auto owner_before(const cycle_gptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.target_ctrl_;
  }

  ///\copydoc cycle_member_ptr::owner_before
  template<typename U>
  auto owner_before(const cycle_member_ptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.get_control();
  }

 private:
  /**
   * \brief Emplacement of a target value.
   * \details Used by cycle_weak_ptr and allocate_cycle to initialize this.
   *
   * Adopts the reference counter that must have been acquired for the target.
   * \pre
   * this->get() == nullptr
   *
   * \pre
   * this has no control block
   *
   * \pre
   * \p new_target_ctrl has had ``store_refs_`` incremented, unless ``new_target_ctrl == nullptr``.
   *
   * \post
   * this->get() == new_target
   *
   * \post
   * control block of this == new_target_ctrl.
   */
  auto emplace_(T* new_target, detail::intrusive_ptr<detail::base_control> new_target_ctrl)
  noexcept
  -> void {
    assert(new_target_ctrl == nullptr || new_target != nullptr);
    assert(target_ctrl_ == nullptr);

    target_ = new_target;
    target_ctrl_ = std::move(new_target_ctrl);
  }

  ///\copydoc cycle_member_ptr::target_
  T* target_ = nullptr;
  ///\brief Control block for this.
  detail::intrusive_ptr<detail::base_control> target_ctrl_ = nullptr;
};


/**
 * \brief Weak cycle pointer.
 * \details
 * A weak pointer does not have ownership of its target,
 * depending on cycle_member_ptr and cycle_gptr keeping its target
 * reachable.
 */
template<typename T>
class cycle_weak_ptr {
  template<typename> friend class cycle_member_ptr;
  template<typename> friend class cycle_gptr;
  template<typename> friend class cycle_weak_ptr;

 public:
  ///\copydoc cycle_member_ptr::element_type
  using element_type = std::remove_extent_t<T>;

  ///\brief Default constructor.
  constexpr cycle_weak_ptr() noexcept {}

  ///\brief Copy constructor.
  cycle_weak_ptr(const cycle_weak_ptr& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {}

  /**
   * \brief Move constructor.
   * \post
   * other.lock() == nullptr
   */
  cycle_weak_ptr(cycle_weak_ptr&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  ///\brief Copy constructor.
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_weak_ptr(const cycle_weak_ptr<U>& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {}

  ///\brief Move constructor.
  ///\post other.lock() == nullptr
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_weak_ptr(cycle_weak_ptr<U>&& other) noexcept
  : target_(std::exchange(other.target_, nullptr)),
    target_ctrl_(other.target_ctrl_.detach(), false)
  {}

  ///\brief Create weak pointer from \p other.
  ///\post this->lock() == other
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_weak_ptr(const cycle_gptr<U>& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.target_ctrl_)
  {}

  ///\brief Create weak pointer from \p other.
  ///\post this->lock() == other
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  cycle_weak_ptr(const cycle_member_ptr<U>& other) noexcept
  : target_(other.target_),
    target_ctrl_(other.get_control())
  {}

  ///\brief Copy assignment.
  auto operator=(const cycle_weak_ptr& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.target_ctrl_;
    return *this;
  }

  ///\brief Move assignment.
  ///\post other.lock() == nullptr
  auto operator=(cycle_weak_ptr&& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = std::exchange(other.target_, nullptr);
    target_ctrl_ = std::move(other.target_ctrl_);
    return *this;
  }

  ///\brief Copy assignment.
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_weak_ptr<U>& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.target_ctrl_;
    return *this;
  }

  ///\brief Move assignment.
  ///\post other.lock() == nullptr
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(cycle_weak_ptr<U>&& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = std::exchange(other.target_, nullptr);
    target_ctrl_ = std::move(other.target_ctrl_);
    return *this;
  }

  ///\brief Assign \p other.
  ///post this->lock() == other
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_gptr<U>& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.target_ctrl_;
    return *this;
  }

  ///\brief Assign \p other.
  ///post this->lock() == other
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(const cycle_member_ptr<U>& other)
  noexcept
  -> cycle_weak_ptr& {
    target_ = other.target_;
    target_ctrl_ = other.get_control();
    return *this;
  }

  ///\brief Reset this pointer.
  ///\post this->lock() == nullptr
  auto reset()
  noexcept
  -> void {
    target_ = nullptr;
    target_ctrl_.reset();
  }

  ///\brief Swap with another weak pointer.
  auto swap(cycle_weak_ptr& other)
  noexcept
  -> void {
    std::swap(target_, other.target_);
    target_ctrl_.swap(other.target_ctrl_);
  }

  ///\brief Test if this weak pointer is expired.
  ///\returns True if this weak pointer is expired, meaning that its pointee has been collected.
  auto expired() const
  noexcept
  -> bool {
    return target_ctrl_ == nullptr || target_ctrl_->expired();
  }

  /**
   * \brief Retrieve cycle_gptr from this.
   * \details
   * Unlike ``cycle_gptr<T>(*this)``, this method does not throw an exception.
   * \returns cycle_gptr holding the value of this, or nullptr if this is expired.
   */
  auto lock() const
  noexcept
  -> cycle_gptr<T> {
    cycle_gptr<T> result;
    if (target_ctrl_ != nullptr && target_ctrl_->weak_acquire())
      result.emplace_(target_, target_ctrl_);
    return result;
  }

  ///\copydoc cycle_member_ptr::owner_before
  template<typename U>
  auto owner_before(const cycle_weak_ptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.target_ctrl_;
  }

  ///\copydoc cycle_member_ptr::owner_before
  template<typename U>
  auto owner_before(const cycle_gptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.target_ctrl_;
  }

  ///\copydoc cycle_member_ptr::owner_before
  template<typename U>
  auto owner_before(const cycle_member_ptr<U>& other) const
  noexcept
  -> bool {
    return target_ctrl_ < other.get_control();
  }

 private:
  ///\brief Target of this weak pointer.
  T* target_ = nullptr;
  ///\brief Control block of this weak pointer.
  detail::intrusive_ptr<detail::base_control> target_ctrl_ = nullptr;
};


///\brief Equality comparison.
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator==(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

///\brief Equality comparison.
///\relates cycle_member_ptr
template<typename T>
inline auto operator==(const cycle_member_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return !x;
}

///\brief Equality comparison.
///\relates cycle_member_ptr
template<typename U>
inline auto operator==(std::nullptr_t x [[maybe_unused]], const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !y;
}

///\brief Inequality comparison.
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator!=(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Inequality comparison.
///\relates cycle_member_ptr
template<typename T>
inline auto operator!=(const cycle_member_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return bool(x);
}

///\brief Inequality comparison.
///\relates cycle_member_ptr
template<typename U>
inline auto operator!=(std::nullptr_t x [[maybe_unused]], const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return bool(y);
}

///\brief Less comparison.
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator<(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

///\brief Less comparison.
///\relates cycle_member_ptr
template<typename T>
inline auto operator<(const cycle_member_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return std::less<typename cycle_member_ptr<T>::element_type*>()(x.get(), nullptr);
}

///\brief Less comparison.
///\relates cycle_member_ptr
template<typename U>
inline auto operator<(std::nullptr_t x [[maybe_unused]], const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return std::less<typename cycle_member_ptr<U>::element_type*>()(nullptr, y.get());
}

///\brief Greater comparison.
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator>(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return y < x;
}

///\brief Greater comparison.
///\relates cycle_member_ptr
template<typename T>
inline auto operator>(const cycle_member_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return nullptr < x;
}

///\brief Greater comparison.
///\relates cycle_member_ptr
template<typename U>
inline auto operator>(std::nullptr_t x [[maybe_unused]], const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return y < nullptr;
}

///\brief Less or equal comparison.
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator<=(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Less or equal comparison.
///\relates cycle_member_ptr
template<typename T>
inline auto operator<=(const cycle_member_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return !(nullptr < x);
}

///\brief Less or equal comparison.
///\relates cycle_member_ptr
template<typename U>
inline auto operator<=(std::nullptr_t x [[maybe_unused]], const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(y < nullptr);
}

///\brief Greater or equal comparison.
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator>=(const cycle_member_ptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

///\brief Greater or equal comparison.
///\relates cycle_member_ptr
template<typename T>
inline auto operator>=(const cycle_member_ptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return !(x < nullptr);
}

///\brief Greater or equal comparison.
///\relates cycle_member_ptr
template<typename U>
inline auto operator>=(std::nullptr_t x [[maybe_unused]], const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(nullptr < y);
}

///\brief Swap two pointers.
///\relates cycle_member_ptr
template<typename T>
inline auto swap(cycle_member_ptr<T>& x, cycle_member_ptr<T>& y)
noexcept
-> void {
  x.swap(y);
}

///\brief Swap two pointers.
///\relates cycle_member_ptr
///\relates cycle_gptr
template<typename T>
inline auto swap(cycle_member_ptr<T>& x, cycle_gptr<T>& y)
noexcept
-> void {
  x.swap(y);
}


///\brief Equality comparison.
///\relates cycle_gptr
template<typename T, typename U>
inline auto operator==(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

///\brief Equality comparison.
///\relates cycle_gptr
template<typename T>
inline auto operator==(const cycle_gptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return !x;
}

///\brief Equality comparison.
///\relates cycle_gptr
template<typename U>
inline auto operator==(std::nullptr_t x [[maybe_unused]], const cycle_gptr<U>& y)
noexcept
-> bool {
  return !y;
}

///\brief Inequality comparison.
///\relates cycle_gptr
template<typename T, typename U>
inline auto operator!=(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Inequality comparison.
///\relates cycle_gptr
template<typename T>
inline auto operator!=(const cycle_gptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return bool(x);
}

///\brief Inequality comparison.
///\relates cycle_gptr
template<typename U>
inline auto operator!=(std::nullptr_t x [[maybe_unused]], const cycle_gptr<U>& y)
noexcept
-> bool {
  return bool(y);
}

///\brief Less comparison.
///\relates cycle_gptr
template<typename T, typename U>
inline auto operator<(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

///\brief Less comparison.
///\relates cycle_gptr
template<typename T>
inline auto operator<(const cycle_gptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return std::less<typename cycle_gptr<T>::element_type*>()(x.get(), nullptr);
}

///\brief Less comparison.
///\relates cycle_gptr
template<typename U>
inline auto operator<(std::nullptr_t x [[maybe_unused]], const cycle_gptr<U>& y)
noexcept
-> bool {
  return std::less<typename cycle_gptr<U>::element_type*>()(nullptr, y.get());
}

///\brief Greater comparison.
///\relates cycle_gptr
template<typename T, typename U>
inline auto operator>(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return y < x;
}

///\brief Greater comparison.
///\relates cycle_gptr
template<typename T>
inline auto operator>(const cycle_gptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return nullptr < x;
}

///\brief Greater comparison.
///\relates cycle_gptr
template<typename U>
inline auto operator>(std::nullptr_t x [[maybe_unused]], const cycle_gptr<U>& y)
noexcept
-> bool {
  return y < nullptr;
}

///\brief Less or equal comparison.
///\relates cycle_gptr
template<typename T, typename U>
inline auto operator<=(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Less or equal comparison.
///\relates cycle_gptr
template<typename T>
inline auto operator<=(const cycle_gptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return !(nullptr < x);
}

///\brief Less or equal comparison.
///\relates cycle_gptr
template<typename U>
inline auto operator<=(std::nullptr_t x [[maybe_unused]], const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(y < nullptr);
}

///\brief Greater or equal comparison.
///\relates cycle_gptr
template<typename T, typename U>
inline auto operator>=(const cycle_gptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

///\brief Greater or equal comparison.
///\relates cycle_gptr
template<typename T>
inline auto operator>=(const cycle_gptr<T>& x, std::nullptr_t y [[maybe_unused]])
noexcept
-> bool {
  return !(x < nullptr);
}

///\brief Greater or equal comparison.
///\relates cycle_gptr
template<typename U>
inline auto operator>=(std::nullptr_t x [[maybe_unused]], const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(nullptr < y);
}

///\brief Swap two pointers.
///\relates cycle_gptr
template<typename T>
inline auto swap(cycle_gptr<T>& x, cycle_gptr<T>& y)
noexcept
-> void {
  x.swap(y);
}

///\brief Swap two pointers.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T>
inline auto swap(cycle_gptr<T>& x, cycle_member_ptr<T>& y)
noexcept
-> void {
  x.swap(y);
}


///\brief Swap two pointers.
///\relates cycle_weak_ptr
template<typename T>
inline auto swap(cycle_weak_ptr<T>& x, cycle_weak_ptr<T>& y)
noexcept
-> void {
  x.swap(y);
}


///\brief Equality comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator==(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

///\brief Equality comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator==(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return x.get() == y.get();
}

///\brief Inequality comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator!=(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Inequality comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator!=(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(x == y);
}

///\brief Less comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator<(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

///\brief Less comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator<(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return x.get() < y.get();
}

///\brief Greater comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator>(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return y < x;
}

///\brief Greater comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator>(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return y < x;
}

///\brief Less or equal comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator<=(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Less or equal comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator<=(const cycle_member_ptr<T>& x, const cycle_gptr<U>& y)
noexcept
-> bool {
  return !(y < x);
}

///\brief Greater or equal comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
template<typename T, typename U>
inline auto operator>=(const cycle_gptr<T>& x, const cycle_member_ptr<U>& y)
noexcept
-> bool {
  return !(x < y);
}

///\brief Greater or equal comparison.
///\relates cycle_gptr
///\relates cycle_member_ptr
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
inline auto allocate_cycle(Alloc alloc, Args&&... args)
-> cycle_gptr<T> {
  using alloc_t = typename std::allocator_traits<Alloc>::template rebind_alloc<T>;
  using control_t = detail::control<T, alloc_t>;
  using alloc_traits = typename std::allocator_traits<Alloc>::template rebind_traits<control_t>;
  using ctrl_alloc_t = typename std::allocator_traits<Alloc>::template rebind_alloc<control_t>;

  ctrl_alloc_t ctrl_alloc = alloc;

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
inline auto make_cycle(Args&&... args)
-> cycle_gptr<T> {
  return allocate_cycle<T>(std::allocator<T>(), std::forward<Args>(args)...);
}


///\brief Write pointer to output stream.
///\relates cycle_member_ptr
template<typename Char, typename Traits, typename T>
inline auto operator<<(std::basic_ostream<Char, Traits>& out, const cycle_member_ptr<T>& ptr)
-> std::basic_ostream<Char, Traits>& {
  return out << ptr.get();
}

///\brief Write pointer to output stream.
///\relates cycle_gptr
template<typename Char, typename Traits, typename T>
inline auto operator<<(std::basic_ostream<Char, Traits>& out, const cycle_gptr<T>& ptr)
-> std::basic_ostream<Char, Traits>& {
  return out << ptr.get();
}


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
  ///\brief When copy-assigning, allocator is not copied over.
  using propagate_on_container_copy_assignment = std::false_type;
  ///\brief When move-assigning, allocator is not copied over.
  using propagate_on_container_move_assignment = std::false_type;
  ///\brief When performing swap, allocator is not swapped.
  using propagate_on_container_swap = std::false_type;
  ///\brief Must check for equality.
  using is_always_equal = std::false_type;

  ///\brief Template for changing controlled type of allocator.
  template<typename T>
  struct rebind {
    ///\brief The type of this allocator, rebound to type \p T.
    using other = cycle_allocator<typename std::allocator_traits<Nested>::template rebind_alloc<T>>;
  };

  ///\brief Copy constructor for distinct type.
  template<typename Other>
  cycle_allocator(const cycle_allocator<Other>& other)
  noexcept(std::is_nothrow_constructible_v<Nested, const Other&>)
  : Nested(other),
    control_(other.control_)
  {}

  ///\brief Create allocator, with declared ownership.
  ///\param base The owner of elements created using this allocator.
  ///\param args Arguments to pass to underlying allocator constructor.
  template<typename... Args, typename = std::enable_if_t<std::is_constructible_v<Nested, Args...>>>
  explicit cycle_allocator(const cycle_base& base, Args&&... args)
  : Nested(std::forward<Args>(args)...),
    control_(base.control_)
  {}

  ///\brief Create allocator, with elements having no ownership.
  ///\param unowned_tag Tag indicating that elements created by this allocator do not have an owning object.
  ///\param args Arguments to pass to underlying allocator constructor.
  template<typename... Args, typename = std::enable_if_t<std::is_constructible_v<Nested, Args...>>>
  explicit cycle_allocator(unowned_cycle_t unowned_tag [[maybe_unused]], Args&&... args)
  : Nested(std::forward<Args>(args)...),
    control_(detail::base_control::unowned_control())
  {}

  /**
   * \brief Constructor.
   * \details
   * Publishes the owner control block prior to construction,
   * to allow for members to pick it up automatically.
   * (After construction, the control block is unpublished.)
   *
   * Forwards to construct as implemented by \p Nested.
   */
  template<typename T, typename... Args>
  auto construct(T* ptr, Args&&... args)
  -> void {
    detail::base_control::publisher pub{ ptr, sizeof(T), *control_ };
    std::allocator_traits<Nested>::construct(*this, ptr, std::forward<Args>(args)...);
  }

  /**
   * \brief Fail to create copy of this allocator.
   * \details
   * Fails a static assert with a message asking you to please explicitly
   * specify what ownership relation to use for copied container.
   *
   * \note
   * Ideally, this would also be possible for move construction of a
   * container, but there's no support for that in the C++ Standard Library.
   */
  template<typename Dummy = void>
  auto select_on_container_copy_construction()
  -> cycle_allocator {
    static_assert(std::is_void_v<Dummy> && false,
        "You must explicitly specify an allocator with owner during copy.");
    return *this;
  }

  /**
   * \brief Compare allocators for equality.
   * \details
   * Two allocators are equal if they use the same owner.
   *
   * (Distinct owners are treated as equal if they represent the unowned
   * control block.)
   */
  auto operator==(const cycle_allocator& other) const
  noexcept(
      std::allocator_traits<Nested>::is_always_equal::value
      || noexcept(std::declval<const Nested&>() == std::declval<const Nested&>()))
  -> bool {
    if constexpr(!std::allocator_traits<Nested>::is_always_equal::value) {
      if (!std::equal_to<Nested>()(*this, other)) return false;
    }

    return control_ == other.control_
        || (control_->is_unowned() && other.control_->is_unowned());
  }

  ///\brief Inequality comparison.
  auto operator!=(const cycle_allocator& other) const
  noexcept(noexcept(std::declval<const cycle_allocator&>() == std::declval<const cycle_allocator&>()))
  -> bool {
    return !(*this == other);
  }

 private:
  ///\brief Control block for ownership.
  detail::intrusive_ptr<detail::base_control> control_;
};


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
inline auto exchange(cycle_ptr::cycle_member_ptr<T>& x, U&& y) {
  cycle_ptr::cycle_gptr<T> result = std::move(x);
  x = std::forward<U>(y);
  return result;
}

/**
 * \brief Hash code implementation for cycle pointers.
 * \details
 * Implements hash code for both cycle_ptr::cycle_member_ptr and cycle_ptr::cycle_gptr,
 * as the two are semantically equivalent.
 * \tparam T The cycle pointer template argument.
 */
template<typename T>
struct hash<cycle_ptr::cycle_member_ptr<T>> {
  ///\brief Argument type.
  ///\deprecated Since C++17.
  [[deprecated]]
  typedef cycle_ptr::cycle_member_ptr<T> argument_type;
  ///\brief Argument type.
  ///\deprecated Since C++17.
  [[deprecated]]
  typedef std::size_t result_type;

  ///\brief Compute the hashcode of a cycle pointer.
  ///\details
  ///If cycle_member_ptr and cycle_gptr point at the
  ///same object, their hash codes shall be the same.
  ///\param p The cycle pointer for which to compute the hash code.
  ///\returns Hashcode of ``p.get()``.
  auto operator()(const cycle_ptr::cycle_member_ptr<T>& p) const
  noexcept
  -> std::size_t {
    return std::hash<typename cycle_ptr::cycle_member_ptr<T>::element_type*>()(p.get());
  }

  ///\brief Compute the hashcode of a cycle pointer.
  ///\details
  ///If cycle_member_ptr and cycle_gptr point at the
  ///same object, their hash codes shall be the same.
  ///\param p The cycle pointer for which to compute the hash code.
  ///\returns Hashcode of ``p.get()``.
  auto operator()(const cycle_ptr::cycle_gptr<T>& p) const
  noexcept
  -> std::size_t {
    return std::hash<typename cycle_ptr::cycle_gptr<T>::element_type*>()(p.get());
  }
};

/**
 * \brief Hash code implementation for cycle pointers.
 * \details
 * Implements hash code for both cycle_ptr::cycle_member_ptr and cycle_ptr::cycle_gptr,
 * as the two are semantically equivalent.
 * \tparam T The cycle pointer template argument.
 */
template<typename T>
struct hash<cycle_ptr::cycle_gptr<T>>
: hash<cycle_ptr::cycle_member_ptr<T>>
{
  ///\brief Argument type.
  ///\deprecated Since C++17.
  [[deprecated]]
  typedef cycle_ptr::cycle_gptr<T> argument_type;
};


///\brief Perform static cast on pointer.
///\relates cycle_ptr::cycle_gptr
template<typename T, typename U>
inline auto static_pointer_cast(const cycle_ptr::cycle_gptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      static_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform dynamic cast on pointer.
///\relates cycle_ptr::cycle_gptr
template<typename T, typename U>
inline auto dynamic_pointer_cast(const cycle_ptr::cycle_gptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      dynamic_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform const cast on pointer.
///\relates cycle_ptr::cycle_gptr
template<typename T, typename U>
inline auto const_pointer_cast(const cycle_ptr::cycle_gptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      const_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform reinterpret cast on pointer.
///\relates cycle_ptr::cycle_gptr
template<typename T, typename U>
inline auto reinterpret_pointer_cast(const cycle_ptr::cycle_gptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      reinterpret_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}


///\brief Perform static cast on pointer.
///\relates cycle_ptr::cycle_member_ptr
template<typename T, typename U>
inline auto static_pointer_cast(const cycle_ptr::cycle_member_ptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      static_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform dynamic cast on pointer.
///\relates cycle_ptr::cycle_member_ptr
template<typename T, typename U>
inline auto dynamic_pointer_cast(const cycle_ptr::cycle_member_ptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      dynamic_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform const cast on pointer.
///\relates cycle_ptr::cycle_member_ptr
template<typename T, typename U>
inline auto const_pointer_cast(const cycle_ptr::cycle_member_ptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      const_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}

///\brief Perform reinterpret cast on pointer.
///\relates cycle_ptr::cycle_member_ptr
template<typename T, typename U>
inline auto reinterpret_pointer_cast(const cycle_ptr::cycle_member_ptr<U>& r)
-> cycle_ptr::cycle_gptr<T> {
  return cycle_ptr::cycle_gptr<T>(
      r,
      reinterpret_cast<typename cycle_ptr::cycle_gptr<T>::element_type*>(r.get()));
}


} /* namespace std */
