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
 * schedule kind.
 *
 * The context exposes the primitives that scheduler-initiated work combines
 * at start(): the eager immediate completion attempt, the worker-local
 * publish fast paths (io_context::publish_io / publish_cpu, which consult
 * running_worker_native() and fall back to the shared queue), and the
 * shared-queue-only publishes (publish_io_deferred / publish_cpu_deferred).
 * Each specialization fixes one combination so the operation layer stays
 * free of per-kind branches: adding a schedule kind means adding one
 * specialization instead of revisiting every operation.
 *
 * The primary template is deliberately left undefined: every schedule_kind
 * enumerator must provide an explicit specialization, so a new kind cannot
 * silently inherit another kind's composition.
 *
 * @tparam Kind The schedule kind whose composition is fixed here.
 */
template <io_context::schedule_kind Kind>
struct schedule_policy;

/**
 * dispatch composition: eager immediate completion is allowed, and
 * publishes follow the worker-local fast path with a shared-queue fallback
 * (io_context::publish_io / io_context::publish_cpu). The dispatch-only
 * inline completion lives in schedule_sender::start(), not here.
 */
template <>
struct schedule_policy<io_context::schedule_kind::dispatch> {
  /** Whether an operation may attempt eager immediate completion. */
  static constexpr bool k_immediate = true;

  /**
   * Publishes I/O through the worker-local fast path or the shared queue.
   *
   * @return false only when the shared path rejected the enqueue because
   *         the context is already stopping; the caller must complete the
   *         operation inline so it never strands.
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
 * post composition: identical publish composition to dispatch, but without
 * the inline completion (schedule_sender::start() never completes inline
 * for post). Kept as its own specialization so each kind's contract is
 * spelled out in one place.
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
 * defer composition: every publish goes through the shared queue only; the
 * eager immediate probe stays enabled.
 *
 * Publishing through the shared queue is what keeps defer-scheduled I/O
 * from being pinned to the publishing worker's connection affinity: the
 * operation is drained by whichever worker reaches it first. The eager
 * probe itself does not reintroduce pinning — a successful probe publishes
 * its completion through the same shared CPU queue — so the probe remains
 * available. k_immediate is still spelled out here so the opt-out seam
 * stays per-kind: flipping this constant moves defer's I/O entirely onto
 * the native submission/completion pipeline without touching the
 * operation layer.
 */
template <>
struct schedule_policy<io_context::schedule_kind::defer> {
  /** Whether an operation may attempt eager immediate completion. */
  static constexpr bool k_immediate = true;

  /**
   * Publishes I/O through the shared queue only.
   *
   * @return false only when the context is already stopping; the caller
   *         must complete the operation inline so it never strands.
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
