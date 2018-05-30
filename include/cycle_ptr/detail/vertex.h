#ifndef CYCLE_PTR_DETAIL_VERTEX_H
#define CYCLE_PTR_DETAIL_VERTEX_H

#include <boost/intrusive_ptr.hpp>
#include <cycle_ptr/detail/llist.h>
#include <cycle_ptr/detail/hazard.h>

namespace cycle_ptr::detail {


class base_control;

class vertex
: public link<vertex>
{
  vertex() = delete;
  vertex(const vertex&) = delete;

 protected:
  explicit vertex(boost::intrusive_ptr<base_control> bc) noexcept
  : bc_(std::move(bc))
  {
    assert(bc_ != nullptr);
    bc_->push_back(*this);
  }

  ~vertex() noexcept {
    assert(this->link<vertex>::linked());
    reset();
    bc_->erase(*this);
  }

  auto reset()
  noexcept
  -> boost::intrusive_ptr<base_control> {
    auto ptr = dst_.exchange(nullptr);
    if (ptr != nullptr) erase_edge(bc_, *ptr);
    return ptr;
  }

  auto reset(const boost::intrusive_ptr<base_control>& new_dst)
  noexcept
  -> boost::intrusive_ptr<base_control> {
    auto ptr = dst_.exchange(new_dst);
    if (new_dst != nullptr) create_edge(bc_, *new_dst);
    if (ptr != nullptr) erase_edge(bc_, *ptr);
    return ptr;
  }

 public:
  ///\brief Read the target control block.
  ///\returns The target control block of this vertex.
  auto get_control() const
  noexcept
  -> boost::intrusive_ptr<base_control> {
    return dst_.load();
  }

 private:
  const boost::intrusive_ptr<base_control> bc_; // Non-null.
  hazard_ptr<base_control> dst_;
};


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_VERTEX_H */
