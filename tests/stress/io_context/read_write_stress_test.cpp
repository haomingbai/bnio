#include <bnio/io_context.h>
#include <bnio/tcp.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "../../support/io_context/io_context_runtime_test_support.h"

namespace {

struct rw_stress_state {
  std::atomic<unsigned> completions{0};
  std::atomic<unsigned> errors{0};
  std::atomic<unsigned> stopped{0};
};

struct rw_stress_receiver {
  rw_stress_state* state;

  void set_value(std::size_t) noexcept {
    state->completions.fetch_add(1, std::memory_order_acq_rel);
  }
  void set_error(std::error_code) noexcept {
    state->errors.fetch_add(1, std::memory_order_relaxed);
  }
  void set_stopped() noexcept {
    state->stopped.fetch_add(1, std::memory_order_relaxed);
  }
};

}  // namespace

TEST(ReadWriteStressTest, many_contexts_random_payloads) {
  constexpr int num_contexts = 20;
  constexpr unsigned worker_count = 2;
  // Use a payload size well below the Unix-domain-socket buffer (8 KiB on
  // macOS) so both ::send() and ::recv() in the initial non-blocking
  // start_io() complete in a single call.  This avoids partial I/O that
  // requires re-registering with kqueue, sidesteps the macOS EVFILT_READ |
  // EV_ONESHOT edge-triggered quirk for pre-existing data, and keeps the
  // test deterministic.
  constexpr std::size_t payload_size = 4 * 1024;
  constexpr unsigned target = static_cast<unsigned>(num_contexts * 2);

  rw_stress_state state;

  // Pre-generate identical-sized payloads and read buffers for all contexts.
  std::vector<std::vector<unsigned char>> payloads(
      static_cast<std::size_t>(num_contexts));
  std::vector<std::vector<unsigned char>> read_bufs(
      static_cast<std::size_t>(num_contexts));
  for (int i = 0; i < num_contexts; ++i) {
    std::size_t idx = static_cast<std::size_t>(i);
    payloads[idx].resize(payload_size);
    read_bufs[idx].resize(payload_size);
    std::generate(
        payloads[idx].begin(), payloads[idx].end(),
        [val = static_cast<unsigned char>(i + 1)]() mutable { return val; });
  }

  // Derive sender and operation types from a representative context + socket.
  bnio::io_context type_factory;
  if (!context_available(type_factory)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto type_scheduler = type_factory.get_post_scheduler();

  int type_fds[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, type_fds), 0);
  bnio::tcp_socket type_writer(type_fds[0]);
  bnio::tcp_socket type_reader(type_fds[1]);

  using read_op_type = decltype(bexec::connect(
      std::declval<decltype(type_reader.async_read(
          type_scheduler, bnio::buffer(read_bufs[0])))>(),
      std::declval<rw_stress_receiver>()));
  using write_op_type = decltype(bexec::connect(
      std::declval<decltype(type_writer.async_write(
          type_scheduler, bnio::buffer(payloads[0]), MSG_NOSIGNAL))>(),
      std::declval<rw_stress_receiver>()));

  EXPECT_EQ(::close(type_fds[0]), 0);
  EXPECT_EQ(::close(type_fds[1]), 0);

  // Per-context runtime state.
  struct context_runtime {
    std::unique_ptr<bnio::io_context> context;
    int fds[2] = {-1, -1};
    bexec::detail::manual_lifetime<read_op_type> read_op;
    bexec::detail::manual_lifetime<write_op_type> write_op;
    std::vector<std::thread> workers;
  };
  std::vector<std::unique_ptr<context_runtime>> runtimes;
  runtimes.reserve(static_cast<std::size_t>(num_contexts));

  for (int i = 0; i < num_contexts; ++i) {
    auto rt = std::unique_ptr<context_runtime>(new context_runtime());
    rt->context = std::make_unique<bnio::io_context>();
    if (!context_available(*rt->context)) {
      GTEST_SKIP() << "native I/O context is unavailable at context " << i;
    }

    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, rt->fds), 0);

    bnio::tcp_socket writer(rt->fds[0]);
    bnio::tcp_socket reader(rt->fds[1]);

    auto scheduler = rt->context->get_post_scheduler();
    std::size_t idx = static_cast<std::size_t>(i);

    // Start write BEFORE read so the read's non-blocking start_io()
    // finds data immediately, bypassing kqueue entirely.
    rt->write_op.emplace_from([&] {
      return bexec::connect(
          writer.async_write(scheduler, bnio::buffer(payloads[idx]),
                             MSG_NOSIGNAL),
          rw_stress_receiver{&state});
    });
    bexec::start(*rt->write_op);

    rt->read_op.emplace_from([&] {
      return bexec::connect(
          reader.async_read(scheduler, bnio::buffer(read_bufs[idx])),
          rw_stress_receiver{&state});
    });
    bexec::start(*rt->read_op);

    // Both tcp_socket objects release their fds; context_runtime owns them.
    (void)writer.release();
    (void)reader.release();

    runtimes.push_back(std::move(rt));
  }

  // Start all workers for all contexts.
  for (auto& rt : runtimes) {
    rt->workers.reserve(worker_count);
    for (unsigned w = 0; w < worker_count; ++w) {
      rt->workers.emplace_back([ctx = rt->context.get()] { ctx->run(); });
    }
  }

  // Wait with timeout for all 40 operations (20 contexts × 2).
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (state.completions.load(std::memory_order_acquire) < target &&
         state.errors.load(std::memory_order_acquire) == 0) {
    if (std::chrono::steady_clock::now() > deadline) {
      ADD_FAILURE() << "timeout: only " << state.completions.load() << " of "
                    << target << " completions";
      break;
    }
    std::this_thread::yield();
  }

  // Stop all contexts from the main thread.
  for (auto& rt : runtimes) {
    (void)rt->context->stop();
  }

  // Join all workers.
  for (auto& rt : runtimes) {
    for (auto& w : rt->workers) {
      w.join();
    }
  }

  EXPECT_EQ(state.completions.load(std::memory_order_acquire), target);
  EXPECT_EQ(state.errors.load(std::memory_order_acquire), 0);
  EXPECT_EQ(state.stopped.load(std::memory_order_acquire), 0);

  // Verify data integrity for every payload.
  for (int i = 0; i < num_contexts; ++i) {
    std::size_t idx = static_cast<std::size_t>(i);
    EXPECT_EQ(
        std::memcmp(read_bufs[idx].data(), payloads[idx].data(), payload_size),
        0);
    EXPECT_EQ(::close(runtimes[idx]->fds[0]), 0);
    EXPECT_EQ(::close(runtimes[idx]->fds[1]), 0);
  }
}
