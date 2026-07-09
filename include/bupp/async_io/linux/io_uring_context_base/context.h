#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_

#include <bupp/async_io/descriptor_view.h>
#include <bupp/async_io/dns.h>
#include <bupp/async_io/linux/io_uring_context_base/operation_base.h>
#include <bupp/async_io/linux/io_uring_context_base/options.h>
#include <bupp/async_io/time.h>
#include <bupp/base/linux/ring.h>
#include <bupp/base/linux/submission_queue_entry.h>
#include <bupp/export.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace bupp::async_io::linux_native {

/**
 * Run loop and submission context backed by a Linux io_uring instance.
 */
class BUPP_EXPORT io_uring_context {
 public:
  /**
   * Monotonic clock used by async I/O scheduling.
   */
  using steady_clock = bupp::async_io::steady_clock;

  /**
   * Clock used as the default async I/O scheduling clock.
   */
  using clock = bupp::async_io::clock;

  /**
   * Wall-clock type for APIs that explicitly need system time.
   */
  using system_clock = bupp::async_io::system_clock;

  /**
   * Canonical async I/O duration.
   */
  using duration = bupp::async_io::duration;

  /**
   * Time point represented with the default async I/O clock.
   */
  using time_point = bupp::async_io::time_point;

  /**
   * System-clock time point represented with async I/O duration precision.
   */
  using system_time_point = bupp::async_io::system_time_point;

#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#include <bupp/async_io/linux/io_uring_context_base/lifecycle_api.h>
#include <bupp/async_io/linux/io_uring_context_base/operation_factory_api.h>
#include <bupp/async_io/linux/io_uring_context_base/run_loop_api.h>
#include <bupp/async_io/linux/io_uring_context_base/submission_api.h>
#include <bupp/async_io/linux/io_uring_context_base/task_queue_api.h>
#undef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_

 private:
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#include <bupp/async_io/linux/io_uring_context_base/cqe_dispatch_state.h>
#include <bupp/async_io/linux/io_uring_context_base/lifecycle_state.h>
#include <bupp/async_io/linux/io_uring_context_base/run_loop_state.h>
#include <bupp/async_io/linux/io_uring_context_base/task_queue_state.h>
#undef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
};

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_
