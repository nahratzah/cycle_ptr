#ifndef CYCLE_PTR_DETAIL_COLOR_H
#define CYCLE_PTR_DETAIL_COLOR_H

#include <cstdint>

namespace cycle_ptr::detail {


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
  red = 0 ///< May or may not be reachable.
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
  return color(refcount & color_mask);
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
      && (get_color(refcounter) == black
          ? get_refs(refcounter) == 0u
          : true);
}


} /* namespace cycle_ptr::detail */

#endif /* CYCLE_PTR_DETAIL_COLOR_H */
