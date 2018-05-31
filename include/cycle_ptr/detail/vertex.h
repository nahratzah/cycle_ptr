#ifndef CYCLE_PTR_DETAIL_VERTEX_H
#define CYCLE_PTR_DETAIL_VERTEX_H

#include <boost/intrusive_ptr.hpp>
#include <cycle_ptr/detail/llist.h>
#include <cycle_ptr/detail/hazard.h>

namespace cycle_ptr::detail {


class base_control;
class generation;

class vertex
: public link<vertex>
{
  friend class generation;
  friend class base_control;

  vertex() = delete;
  vertex(const vertex&) = delete;

 protected:
  explicit vertex(boost::intrusive_ptr<base_control> bc) noexcept;
  ~vertex() noexcept;

  auto reset() -> void;

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
      boost::intrusive_ptr<base_control> new_dst,
      bool has_reference,
      bool no_red_promotion)
  -> void;

  ///\brief Test if origin is expired.
  auto owner_is_expired() const noexcept -> bool;

  ///\brief Throw exception if owner is expired.
  ///\details
  ///It is not allowed to read member pointers from an expired object.
  ///\todo Create dedicated exception for this case.
  auto throw_if_owner_expired() const
  -> void {
    if (owner_is_expired()) throw std::bad_weak_ptr();
  }

 public:
  ///\brief Read the target control block.
  ///\returns The target control block of this vertex.
  auto get_control() const noexcept -> boost::intrusive_ptr<base_control>;

 private:
  const boost::intrusive_ptr<base_control> bc_; // Non-null.
  hazard_ptr<base_control> dst_;
};


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_VERTEX_H */
