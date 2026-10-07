// Pinned contract for the local-socket split
// (docs/design/architecture/local-socket-split.md §7.2): the AF_UNIX stream
// pins below must survive the split unchanged — only the fd wrapping
// migrates (stream_socket_view → async_io::local::stream_socket_view).
//
// Contract pinned here: error-face termination of write-all (async_write)
// when the peer of an AF_UNIX SOCK_STREAM pair closes.
//
//   1. Peer close mid-transfer: the write-all terminates with exactly one
//      completion (set_value), the error is EPIPE (std::generic_category,
//      MSG_NOSIGNAL suppresses SIGPIPE), never set_stopped, and the
//      completion preserves the byte count transferred before the close.
//   2. The eager runtime toggle (enable_immediate_io) does not change the
//      observable outcome (§3.6 of the design doc).
//   3. Peer already closed before the first write attempt: the immediate
//      probe surfaces EPIPE with zero bytes transferred, still exactly one
//      completion.
//
// No existing test closes the peer during an in-flight socketpair write:
// io_context_read_write_test.cpp covers stop()- and stop-token-driven
// termination only, so these pins close the error-face gap.
//
// All waits are event-driven (completion flag / deadline-bounded yield
// loops); no sleeps synchronize the scenario.

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

#include "../../support/io_context/io_context_runtime_test_support.h"

#if !defined(SOCK_CLOEXEC)
#define SOCK_CLOEXEC 0
#endif

namespace {

// Record of the single terminal receiver call. `calls` counts every terminal
// call so a double completion is caught even after `done` is published.
struct completion_record {
  std::atomic<unsigned> calls{0};
  std::atomic<bool> done{false};
  std::atomic<bool> stopped{false};
  // Written once before done.store(release); read after done.load(acquire).
  std::error_code error{};
  std::size_t bytes = 0;
};

struct write_contract_receiver {
  completion_record* record;
  bnio::io_context* context;  // may be null: caller drives run() externally

  void set_value(std::error_code ec, std::size_t size) noexcept {
    record->calls.fetch_add(1, std::memory_order_acq_rel);
    record->error = ec;
    record->bytes = size;
    record->done.store(true, std::memory_order_release);
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    record->calls.fetch_add(1, std::memory_order_acq_rel);
    record->stopped.store(true, std::memory_order_relaxed);
    record->done.store(true, std::memory_order_release);
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

[[nodiscard]] std::chrono::steady_clock::time_point deadline_in(
    std::chrono::seconds seconds) {
  return std::chrono::steady_clock::now() + seconds;
}

// Waits until the record is published or the deadline passes.
[[nodiscard]] bool wait_done(const completion_record& record,
                             std::chrono::steady_clock::time_point deadline) {
  while (!record.done.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// Drains exactly `quota` bytes from an O_NONBLOCK-style raw recv loop, then
// closes the descriptor. Returns false if the quota could not be drained
// before the deadline (the mid-close precondition was not exercised) or an
// unexpected recv result appeared; the descriptor is still closed then.
[[nodiscard]] bool drain_then_close(
    int fd, std::size_t quota, std::chrono::steady_clock::time_point deadline) {
  std::size_t received = 0;
  std::array<char, 4096> scratch{};
  while (received < quota) {
    if (std::chrono::steady_clock::now() >= deadline) {
      (void)::close(fd);
      return false;
    }
    const ssize_t n = ::recv(fd, scratch.data(), scratch.size(), MSG_DONTWAIT);
    if (n > 0) {
      received += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::yield();
    } else {
      // n == 0 (EOF) or another errno: not part of the pinned scenario.
      (void)::close(fd);
      return false;
    }
  }
  return ::close(fd) == 0;
}

void run_peer_close_midway(bool eager) {
  bnio::io_context_options options;
  options.enable_immediate_io = eager;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
  // This is the pre-split wrapping; it migrates to
  // bnio::async_io::local::stream_socket_view with the local-socket split.
  const bnio::async_io::stream_socket_view writer(fds[0]);

  // 1 MiB payload far exceeds the AF_UNIX send buffer, so the write-all is
  // guaranteed to still be in flight when the peer closes.
  constexpr std::size_t kPayloadSize = 1U << 20;  // 1 MiB
  // The reader consumes less than the payload, then closes: the writer must
  // terminate mid-payload with partial progress preserved.
  constexpr std::size_t kReaderQuota = 64U * 1024;

  std::vector<unsigned char> payload(kPayloadSize);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<unsigned char>(i & 0xff);
  }

  completion_record record;
  auto operation = bexec::connect(
      scheduler.async_write(
          writer,
          bnio::buffer(static_cast<const unsigned char*>(payload.data()),
                       payload.size()),
          MSG_NOSIGNAL),
      write_contract_receiver{&record, &context});
  bexec::start(operation);

  const auto deadline = deadline_in(std::chrono::seconds(8));
  std::atomic<bool> reader_ok{false};
  std::thread reader([&] {
    reader_ok.store(drain_then_close(fds[1], kReaderQuota, deadline),
                    std::memory_order_release);
  });
  std::thread worker([&context] { context.run(); });

  if (!wait_done(record, deadline)) {
    ADD_FAILURE() << "timeout: write-all did not terminate after peer close";
    (void)context.stop();
    reader.join();
    worker.join();
    (void)::close(fds[1]);
    (void)::close(fds[0]);
    return;
  }
  reader.join();
  worker.join();

  ASSERT_TRUE(reader_ok.load(std::memory_order_acquire))
      << "reader failed to drain quota and close the peer mid-transfer";

  // Contract: exactly one completion, error class EPIPE, never set_stopped,
  // partial byte count preserved.
  EXPECT_EQ(record.calls.load(std::memory_order_acquire), 1U);
  EXPECT_FALSE(record.stopped.load(std::memory_order_acquire));
  EXPECT_EQ(record.error, std::error_code(EPIPE, std::generic_category()));
  EXPECT_GT(record.bytes, 0U);

  (void)::close(fds[0]);
}

}  // namespace

// Pinned contract: peer close mid-transfer terminates the in-flight write-all
// with exactly one EPIPE completion carrying the bytes transferred so far
// (eager runtime switch on, the default model selection).
TEST(LocalStreamPeerCloseTest, write_all_peer_close_midway_reports_epipe_once) {
  run_peer_close_midway(/*eager=*/true);
}

// Same contract with the eager runtime switch off: the termination flows
// through the registered (non-immediate) send path instead of the eager
// probe, and the observable outcome must be identical.
TEST(LocalStreamPeerCloseTest,
     write_all_peer_close_midway_reports_epipe_once_eager_off) {
  run_peer_close_midway(/*eager=*/false);
}

// Pinned contract: with the peer closed before the first write attempt, the
// immediate probe surfaces EPIPE synchronously with zero bytes transferred,
// still exactly one completion and never set_stopped.
TEST(LocalStreamPeerCloseTest,
     write_all_peer_closed_before_start_reports_epipe_once) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
  const bnio::async_io::stream_socket_view writer(fds[0]);

  constexpr std::size_t kPayloadSize = 1U << 20;
  std::vector<unsigned char> payload(kPayloadSize, 'x');

  // Close the peer before any write is attempted.
  ASSERT_EQ(::close(fds[1]), 0);

  completion_record record;
  auto operation = bexec::connect(
      scheduler.async_write(
          writer,
          bnio::buffer(static_cast<const unsigned char*>(payload.data()),
                       payload.size()),
          MSG_NOSIGNAL),
      write_contract_receiver{&record, &context});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(record.calls.load(std::memory_order_acquire), 1U);
  EXPECT_FALSE(record.stopped.load(std::memory_order_acquire));
  EXPECT_EQ(record.error, std::error_code(EPIPE, std::generic_category()));
  EXPECT_EQ(record.bytes, 0U);

  (void)::close(fds[0]);
}
