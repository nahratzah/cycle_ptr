#ifndef CYCLE_PTR_CYCLE_PTR_H
#define CYCLE_PTR_CYCLE_PTR_H

#include <cassert>
#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <iosfwd>
#include <cycle_ptr/detail/intrusive_ptr.h>
#include <cycle_ptr/detail/control.h>
#include <cycle_ptr/detail/vertex.h>

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
  cycle_base([[maybe_unused]] unowned_cycle_t unowned_tag)
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
  cycle_base(const cycle_base&)
  noexcept
  : cycle_base()
  {}

  /**
   * \brief Copy assignment.
   * \details
   * A noop, provided so you don't lose default assignment in derived classes.
   */
  auto operator=(const cycle_base&)
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
  explicit cycle_member_ptr([[maybe_unused]] unowned_cycle_t unowned_tag) noexcept
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
  cycle_member_ptr(unowned_cycle_t unowned_tag, [[maybe_unused]] std::nullptr_t nil) noexcept
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
  cycle_member_ptr([[maybe_unused]] unowned_cycle_t unowned_tag, const cycle_member_ptr<U>& ptr)
  : detail::vertex(detail::base_control::unowned_control()),
    target_(ptr.target_)
  {
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
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
  cycle_member_ptr(unowned_cycle_t unowned_tag, cycle_member_ptr<U>&& ptr)
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
  cycle_member_ptr([[maybe_unused]] unowned_cycle_t unowned_tag, const cycle_gptr<U>& ptr)
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
  cycle_member_ptr([[maybe_unused]] unowned_cycle_t unowned_tag, cycle_gptr<U>&& ptr)
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
   * \throws std::bad_weak_ptr If ptr is owned by an expired owner.
   *
   * \bug It looks like std::shared_ptr allows similar aliases to be
   * constructed without \p ptr having ownership of anything, yet still
   * pointing at \p target.
   * This case is currently unspecified in cycle_ptr library.
   */
  template<typename U>
  cycle_member_ptr([[maybe_unused]] unowned_cycle_t unowned_tag, const cycle_member_ptr<U>& ptr, element_type* target)
  : detail::vertex(detail::base_control::unowned_control()),
    target_(target)
  {
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
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
  cycle_member_ptr([[maybe_unused]] unowned_cycle_t unowned_tag, const cycle_gptr<U>& ptr, element_type* target)
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
   */
  cycle_member_ptr(cycle_base& owner, [[maybe_unused]] std::nullptr_t nil) noexcept
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
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
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
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
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
  cycle_member_ptr([[maybe_unused]] std::nullptr_t nil) {}

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
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
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
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
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
    ptr.throw_if_owner_expired();
    this->detail::vertex::reset(ptr.get_control(), false, false);
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
  auto operator=([[maybe_unused]] std::nullptr_t nil)
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
  -> cycle_member_ptr& {
    other.throw_if_owner_expired();

    this->detail::vertex::reset(other.get_control(), false, false);
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
   * \throws std::runtime_error if the owner of other is expired.
   */
  auto operator=(cycle_member_ptr&& other)
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
  -> cycle_member_ptr& {
    other.throw_if_owner_expired();

    this->detail::vertex::reset(other.get_control(), false, false);
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
   * \throws std::runtime_error if the owner of other is expired.
   */
  template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  auto operator=(cycle_member_ptr<U>&& other)
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
  -> T* {
    throw_if_owner_expired();
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
  -> std::enable_if_t<Enable, T>* {
    assert(get() != nullptr);
    return get();
  }

  /**
   * \brief Test if this pointer points holds a non-nullptr value.
   * \returns get() != nullptr
   * \throws std::runtime_error if the owner of this is expired.
   */
  explicit operator bool() const {
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
  constexpr cycle_gptr([[maybe_unused]] std::nullptr_t nil) noexcept
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
    other.throw_if_owner_expired();
    if (target_ctrl_ != nullptr) target_ctrl_->acquire();
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
    other.throw_if_owner_expired();
    if (target_ctrl_ != nullptr) target_ctrl_->acquire();
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
    other.throw_if_owner_expired();

    detail::intrusive_ptr<detail::base_control> bc = other.get_control();
    if (bc != nullptr) bc->acquire();

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
auto allocate_cycle(Alloc alloc, Args&&... args)
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
auto make_cycle(Args&&... args)
-> cycle_gptr<T> {
  return allocate_cycle<T>(std::allocator<T>(), std::forward<Args>(args)...);
}


///\brief Write pointer to output stream.
///\relates cycle_member_ptr
template<typename Char, typename Traits, typename T>
auto operator<<(std::basic_ostream<Char, Traits>& out, const cycle_member_ptr<T>& ptr)
-> std::basic_ostream<Char, Traits>& {
  return out << ptr.get();
}

///\brief Write pointer to output stream.
///\relates cycle_gptr
template<typename Char, typename Traits, typename T>
auto operator<<(std::basic_ostream<Char, Traits>& out, const cycle_gptr<T>& ptr)
-> std::basic_ostream<Char, Traits>& {
  return out << ptr.get();
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

/**
 * \brief Hash code implementation for cycle pointers.
 * \details
 * Implements hash code for both cycle_ptr::cycle_member_ptr and cycle_ptr::cycle_gptr,
 * as the two are semantically equivalent.
 * \tparam T The cycle pointer template argument.
 */
template<typename T>
struct hash<cycle_ptr::cycle_member_ptr<T>> {
  [[deprecated]]
  typedef cycle_ptr::cycle_member_ptr<T> argument_type;
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
  [[deprecated]]
  typedef cycle_ptr::cycle_gptr<T> argument_type;
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
