#ifndef CYCLE_PTR_UTIL_H
#define CYCLE_PTR_UTIL_H

///\file
///\brief cycle_ptr utility functions.

#include <functional>
#include <cycle_ptr/detail/intrusive_ptr.h>
#include <cycle_ptr/detail/generation.h>

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

#endif /* CYCLE_PTR_UTIL_H */
