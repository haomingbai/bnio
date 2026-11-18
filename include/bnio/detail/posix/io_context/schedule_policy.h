/**
 * @file schedule_policy.h
 * @brief Per-kind composition of the io_context publish primitives.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_SCHEDULE_POLICY_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_SCHEDULE_POLICY_H_

namespace bnio::detail {

/**
 * Compile-time composition of the io_context publish primitives for one
 * schedule kind: eager immediate completion, the publish_io / publish_cpu
 * fast paths, and the shared-queue-only deferred variants. Each kind fixes
 * one combination so the operation layer has no per-kind branches; the
 * primary template is undefined, so every schedule_kind must specialize it.
 *
 * @tparam Kind The schedule kind whose composition is fixed here.
 */
template <io_context::schedule_kind Kind>
struct schedule_policy;

/**
 * dispatch: eager completion allowed; publishes use the worker-local fast
 * path with a shared-queue fallback. Inline completion lives in
 * schedule_sender::start().
 */
template <>
struct schedule_policy<io_context::schedule_kind::dispatch> {
  /** Whether an operation may attempt eager immediate completion. */
  static constexpr bool k_immediate = true;

  /**
   * Publishes I/O through the worker-local fast path or the shared queue.
   *
   * @return false only when the context is already stopping; the caller
   *         must then complete the operation inline.
   */
  [[nodiscard]] static bool publish_io(
      io_context& context, io_context::operation_base& operation) noexcept {
    return context.publish_io(operation);
  }

  /**
   * Publishes CPU work through the worker-local fast path or the shared
   * queue; see publish_io() for the return contract.
   */
  [[nodiscard]] static bool publish_cpu(
      io_context& context, native_operation_base& operation) noexcept {
    return context.publish_cpu(operation);
  }
};

/**
 * post: same publish composition as dispatch, without the dispatch inline
 * fast path; a publish rejected by a stopping context still completes inline.
 */
template <>
struct schedule_policy<io_context::schedule_kind::post> {
  /** Whether an operation may attempt eager immediate completion. */
  static constexpr bool k_immediate = true;

  /**
   * Publishes I/O through the worker-local fast path or the shared queue;
   * see the dispatch specialization for the return contract.
   */
  [[nodiscard]] static bool publish_io(
      io_context& context, io_context::operation_base& operation) noexcept {
    return context.publish_io(operation);
  }

  /**
   * Publishes CPU work through the worker-local fast path or the shared
   * queue; see the dispatch specialization for the return contract.
   */
  [[nodiscard]] static bool publish_cpu(
      io_context& context, native_operation_base& operation) noexcept {
    return context.publish_cpu(operation);
  }
};

/**
 * defer: every publish goes through the shared queue only, so the operation
 * is drained by whichever worker reaches it first and is not pinned to the
 * publishing worker's affinity. The eager probe stays enabled because a
 * successful probe publishes through the same shared CPU queue.
 */
template <>
struct schedule_policy<io_context::schedule_kind::defer> {
  /** Whether an operation may attempt eager immediate completion. */
  static constexpr bool k_immediate = true;

  /**
   * Publishes I/O through the shared queue only.
   *
   * @return false only when the context is already stopping; the caller
   *         must then complete the operation inline.
   */
  [[nodiscard]] static bool publish_io(
      io_context& context, io_context::operation_base& operation) noexcept {
    return context.publish_io_deferred(operation);
  }

  /**
   * Publishes CPU work through the shared queue only; see publish_io()
   * for the return contract.
   */
  [[nodiscard]] static bool publish_cpu(
      io_context& context, native_operation_base& operation) noexcept {
    return context.publish_cpu_deferred(operation);
  }
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_SCHEDULE_POLICY_H_
