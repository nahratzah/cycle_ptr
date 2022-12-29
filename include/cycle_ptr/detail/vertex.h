#ifndef CYCLE_PTR_DETAIL_VERTEX_H
#define CYCLE_PTR_DETAIL_VERTEX_H

#include <cycle_ptr/detail/export_.h>
#include <cycle_ptr/detail/intrusive_ptr.h>
#include <cycle_ptr/detail/llist.h>
#include <cycle_ptr/detail/hazard.h>
#include <memory>

namespace cycle_ptr::detail {


class base_control;
class generation;

class vertex
: public link<vertex>
{
  friend class generation;
  friend class base_control;

 protected:
  cycle_ptr_export_
  vertex();

  vertex(const vertex& other [[maybe_unused]])
  : vertex()
  {}

  cycle_ptr_export_
  explicit vertex(intrusive_ptr<base_control> bc) noexcept;
  cycle_ptr_export_
  ~vertex() noexcept;

  cycle_ptr_export_
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
  cycle_ptr_export_
  auto reset(
      intrusive_ptr<base_control> new_dst,
      bool has_reference,
      bool no_red_promotion) noexcept
  -> void;

  ///\brief Test if origin is expired.
  cycle_ptr_export_
  auto owner_is_expired() const noexcept -> bool;

 public:
  ///\brief Read the target control block.
  ///\returns The target control block of this vertex.
  cycle_ptr_export_
  auto get_control() const noexcept -> intrusive_ptr<base_control>;

 private:
  const intrusive_ptr<base_control> bc_; // Non-null.
  hazard_ptr<base_control> dst_;
};


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_VERTEX_H */
