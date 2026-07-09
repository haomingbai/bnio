#pragma once
#ifndef BUPP_LINUX_IO_CONTEXT_H_
#define BUPP_LINUX_IO_CONTEXT_H_

#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/async_io/time.h>
#include <bupp/buffer.h>
#include <bupp/export.h>
#include <bupp/ip.h>
#include <bupp/linux/detail/io_context_options.h>
#include <bupp/linux/detail/io_context_timer_types.h>
#include <bupp/linux/detail/steady_timer.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bupp {

enum class ssl_handshake_type;

namespace base {
class submission_queue_entry;
}  // namespace base

/**
 * High-level asynchronous I/O context for Linux.
 *
 * io_context adapts the non-owning async_io views into sender-returning
 * operations. Higher-level stream owners build on top of these view-level
 * operations instead of being known by io_context.
 */
class BUPP_EXPORT io_context {
 public:
#define BUPP_LINUX_IO_CONTEXT_CLASS_SCOPE_
#include <bupp/linux/detail/io_context_operation_members.h>
#include <bupp/linux/detail/io_context_scheduler_members.h>
#undef BUPP_LINUX_IO_CONTEXT_CLASS_SCOPE_

  /**
   * Creates a context with default options.
   */
  io_context() noexcept;

  /**
   * Creates a context with explicit options.
   */
  explicit io_context(const io_context_options& options) noexcept;

  /**
   * Stops the flush timer and releases context resources.
   */
  ~io_context() noexcept;

  /**
   * Copy construction is disabled because the context owns an io_uring context
   * and synchronization resources.
   */
  io_context(const io_context&) = delete;

  /**
   * Copy assignment is disabled because the context owns an io_uring context
   * and synchronization resources.
   */
  io_context& operator=(const io_context&) = delete;

  /**
   * Move construction is disabled because the context owns synchronization
   * resources and timer state.
   */
  io_context(io_context&&) = delete;

  /**
   * Move assignment is disabled because the context owns synchronization
   * resources and timer state.
   */
  io_context& operator=(io_context&&) = delete;

  /**
   * Returns whether the underlying native context is open.
   */
  [[nodiscard]] bool is_open() const noexcept;

  /**
   * Runs the context event loop.
   */
  void run() noexcept;

  /**
   * Requests the context event loop to stop.
   */
  int stop() noexcept;

  /**
   * Returns whether the current thread is running this context.
   */
  [[nodiscard]] bool is_in_context() const noexcept;

  /**
   * Returns a scheduler whose schedule() may complete inline on the context
   * thread.
   */
  [[nodiscard]] dispatch_scheduler get_dispatch_scheduler() noexcept;

  /**
   * Returns a scheduler whose schedule() always posts through the context loop.
   */
  [[nodiscard]] post_scheduler get_post_scheduler() noexcept;

 private:
#define BUPP_LINUX_IO_CONTEXT_CLASS_SCOPE_
#include <bupp/linux/detail/io_context_private_io_members.h>
#include <bupp/linux/detail/io_context_timer_members.h>
#undef BUPP_LINUX_IO_CONTEXT_CLASS_SCOPE_
};

}  // namespace bupp

// clang-format off
#include <bupp/io_context_cpo.h>
#include <bupp/linux/detail/io_context_native_io.h>
#include <bupp/detail/io_context/scheduler_operations.h>
// clang-format on

#endif  // BUPP_LINUX_IO_CONTEXT_H_
