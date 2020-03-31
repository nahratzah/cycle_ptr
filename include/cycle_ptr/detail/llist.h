#ifndef CYCLE_PTR_DETAIL_LLIST_H
#define CYCLE_PTR_DETAIL_LLIST_H

#include <cassert>
#include <utility>
#include <iterator>
#include <initializer_list>

namespace cycle_ptr::detail {


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
auto llist<T, Tag>::insert(const_iterator pos, T& v)
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
auto llist<T, Tag>::insert(const_iterator pos, Iter b, Iter e)
-> iterator {
  assert(pos.link_ != nullptr);
  if (b == e) return iterator(const_cast<link<Tag>*>(pos.link_));

  iterator result = insert(pos, *b);
  while (++b != e) insert(pos, *b);
  return result;
}

template<typename T, typename Tag>
auto llist<T, Tag>::erase(const_iterator b)
-> iterator {
  assert(b != end());
  return erase(b, std::next(b));
}

template<typename T, typename Tag>
auto llist<T, Tag>::erase(const_iterator b, const_iterator e)
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
auto llist<T, Tag>::splice(const_iterator pos, llist& other)
noexcept
-> void {
  assert(&other != this);
  splice(pos, other, other.begin(), other.end());
}

template<typename T, typename Tag>
auto llist<T, Tag>::splice(const_iterator pos, llist& other, const_iterator elem)
noexcept
-> void {
  assert(pos.link_ != nullptr && elem.link_ != nullptr);
  if (elem.link_ == pos.link_) return; // Insert before self.
  splice(pos, other, elem, std::next(elem));
}

template<typename T, typename Tag>
auto llist<T, Tag>::splice(const_iterator pos, llist& other, const_iterator other_begin, const_iterator other_end)
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


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_LLIST_H */
