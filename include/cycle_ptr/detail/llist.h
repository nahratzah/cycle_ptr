#ifndef CYCLE_PTR_DETAIL_LLIST_H
#define CYCLE_PTR_DETAIL_LLIST_H

#include <cassert>
#include <utility>
#include <iterator>
#include <initializer_list>

namespace cycle_ptr::detail {


struct llist_head_tag {};

template<typename> class link;


template<typename T, typename Tag>
class llist
: private link<Tag>
{
  static_assert(std::is_base_of_v<link<Tag>, T>,
      "Require T to derive from link<Tag>.");

 public:
  using value_type = T;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using size_type = std::uintptr_t;

  class iterator;
  class const_iterator;

  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  llist() noexcept
  : link<Tag>(llist_head_tag())
  {}

  llist(const llist&) = delete;
  auto operator=(const llist&) -> llist& = delete;

  llist(llist&& other) noexcept
  : link<Tag>()
  {
    splice(end(), other);
  }

  template<typename Iter>
  llist(Iter b, Iter e)
  : llist()
  {
    insert(end(), b, e);
  }

  ~llist() noexcept {
    clear();
  }

  auto empty() const
  noexcept
  -> bool {
    return this->pred_ == this;
  }

  auto size() const
  noexcept
  -> size_type {
    return static_cast<size_type>(std::distance(begin(), end()));
  }

  auto clear()
  noexcept
  -> void {
    erase(begin(), end());
  }

  static auto iterator_to(T& elem)
  noexcept
  -> iterator {
    return iterator(std::addressof(elem));
  }

  static auto iterator_to(const T& elem)
  noexcept
  -> const_iterator {
    return const_iterator(std::addressof(elem));
  }

  auto front()
  -> T& {
    assert(!empty());
    return *begin();
  }

  auto front() const
  -> const T& {
    assert(!empty());
    return *begin();
  }

  auto back()
  -> T& {
    assert(!empty());
    return *std::prev(end());
  }

  auto back() const
  -> const T& {
    assert(!empty());
    return *std::prev(end());
  }

  auto begin()
  noexcept
  -> iterator {
    return iterator(this->succ_);
  }

  auto begin() const
  noexcept
  -> const_iterator {
    return cbegin();
  }

  auto cbegin() const
  noexcept
  -> const_iterator {
    return const_iterator(this->succ_);
  }

  auto end()
  noexcept
  -> iterator {
    return iterator(this);
  }

  auto end() const
  noexcept
  -> const_iterator {
    return cend();
  }

  auto cend() const
  noexcept
  -> const_iterator {
    return const_iterator(this);
  }

  auto rbegin()
  noexcept
  -> reverse_iterator {
    return reverse_iterator(end());
  }

  auto rbegin() const
  noexcept
  -> const_reverse_iterator {
    return crbegin();
  }

  auto crbegin() const
  noexcept
  -> const_reverse_iterator {
    return const_reverse_iterator(cend());
  }

  auto rend()
  noexcept
  -> reverse_iterator {
    return reverse_iterator(begin());
  }

  auto rend() const
  noexcept
  -> const_reverse_iterator {
    return crend();
  }

  auto crend() const
  noexcept
  -> const_reverse_iterator {
    return const_reverse_iterator(cbegin());
  }

  auto push_back(T& v)
  noexcept
  -> void {
    insert(end(), v);
  }

  auto push_front(T& v)
  noexcept
  -> void {
    insert(begin(), v);
  }

  auto insert(const_iterator pos, T& v) noexcept -> iterator;

  template<typename Iter>
  auto insert(const_iterator pos, Iter b, Iter e) -> iterator;

  auto pop_front()
  -> T& {
    assert(!empty());
    T& result = front();
    erase(begin());
    return result;
  }

  auto pop_back()
  -> T& {
    assert(!empty());
    T& result = back();
    erase(std::prev(end()));
    return result;
  }

  auto erase(const_iterator b) -> iterator;
  auto erase(const_iterator b, const_iterator e) -> iterator;
  auto splice(const_iterator pos, llist& other) noexcept -> void;
  auto splice(const_iterator pos, llist& other, const_iterator elem) noexcept -> void;
  auto splice(const_iterator pos, llist& other, const_iterator other_begin, const_iterator other_end) noexcept -> void;
};


template<typename Tag>
class link {
  template<typename, typename> friend class llist;

 public:
  constexpr link() noexcept = default;

#ifndef NDEBUG
  ~link() noexcept {
    assert((pred_ == nullptr && succ_ == nullptr)
        || (pred_ == this && succ_ == this));
  }
#else
  ~link() noexcept = default;
#endif

 protected:
  constexpr link([[maybe_unused]] const link& other) noexcept
  : link()
  {}

  constexpr auto operator=([[maybe_unused]] const link& other) noexcept
  -> link& {
    return *this;
  }

 private:
  link([[maybe_unused]] const llist_head_tag& lht) noexcept
  : pred_(this),
    succ_(this)
  {}

 protected:
  constexpr auto linked() const
  noexcept
  -> bool {
    return pred_ != nullptr;
  }

 private:
  link* pred_ = nullptr;
  link* succ_ = nullptr;
};


template<typename T, typename Tag>
class llist<T, Tag>::iterator {
  template<typename, typename> friend class llist;
  friend class llist::const_iterator;

 public:
  using value_type = T;
  using reference = T&;
  using pointer = T*;
  using difference_type = std::intptr_t;
  using iterator_category = std::bidirectional_iterator_tag;

  constexpr iterator() noexcept = default;

 private:
  explicit constexpr iterator(link<Tag>* ptr) noexcept
  : link_(ptr)
  {
    assert(ptr != nullptr);
  }

 public:
  auto operator*() const
  noexcept
  -> T& {
    assert(link_ != nullptr);
    return *static_cast<T*>(link_);
  }

  auto operator->() const
  noexcept
  -> T* {
    assert(link_ != nullptr);
    return static_cast<T*>(link_);
  }

  auto operator++()
  noexcept
  -> iterator& {
    assert(link_ != nullptr);
    link_ = link_->succ_;
    return *this;
  }

  auto operator--()
  noexcept
  -> iterator& {
    assert(link_ != nullptr);
    link_ = link_->pred_;
    return *this;
  }

  auto operator++(int)
  noexcept
  -> iterator {
    iterator result = *this;
    ++*this;
    return result;
  }

  auto operator--(int)
  noexcept
  -> iterator {
    iterator result = *this;
    --*this;
    return result;
  }

  auto operator==(const iterator& other) const
  noexcept
  -> bool {
    return link_ == other.link_;
  }

  auto operator!=(const iterator& other) const
  noexcept
  -> bool {
    return !(*this == other);
  }

  auto operator==(const const_iterator& other) const
  noexcept
  -> bool {
    return link_ == other.link_;
  }

  auto operator!=(const const_iterator& other) const
  noexcept
  -> bool {
    return !(*this == other);
  }

 private:
  link<Tag>* link_ = nullptr;
};

template<typename T, typename Tag>
class llist<T, Tag>::const_iterator {
  template<typename, typename> friend class llist;
  friend class llist::iterator;

 public:
  using value_type = T;
  using reference = const T&;
  using pointer = const T*;
  using difference_type = std::intptr_t;
  using iterator_category = std::bidirectional_iterator_tag;

 private:
  explicit constexpr const_iterator(const link<Tag>* ptr) noexcept
  : link_(ptr)
  {
    assert(ptr != nullptr);
  }

 public:
  constexpr const_iterator() noexcept = default;

  constexpr const_iterator(const iterator& other) noexcept
  : link_(other.link_)
  {}

  constexpr auto operator=(const iterator& other)
  noexcept
  -> const_iterator& {
    link_ = other.link_;
    return *this;
  }

  auto operator*() const
  noexcept
  -> const T& {
    assert(link_ != nullptr);
    return *static_cast<const T*>(link_);
  }

  auto operator->() const
  noexcept
  -> const T* {
    assert(link_ != nullptr);
    return static_cast<const T*>(link_);
  }

  auto operator++()
  noexcept
  -> const_iterator& {
    assert(link_ != nullptr);
    link_ = link_->succ_;
    return *this;
  }

  auto operator--()
  noexcept
  -> const_iterator& {
    assert(link_ != nullptr);
    link_ = link_->pred_;
    return *this;
  }

  auto operator++(int)
  noexcept
  -> const_iterator {
    const_iterator result = *this;
    ++*this;
    return result;
  }

  auto operator--(int)
  noexcept
  -> const_iterator {
    const_iterator result = *this;
    --*this;
    return result;
  }

  auto operator==(const const_iterator& other) const
  noexcept
  -> bool {
    return link_ == other.link_;
  }

  auto operator!=(const const_iterator& other) const
  noexcept
  -> bool {
    return !(*this == other);
  }

  auto operator==(const iterator& other) const
  noexcept
  -> bool {
    return link_ == other.link_;
  }

  auto operator!=(const iterator& other) const
  noexcept
  -> bool {
    return !(*this == other);
  }

 private:
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

  other_succ->succ_ = other_pred;
  other_pred->pred_ = other_succ;
}


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_LLIST_H */
