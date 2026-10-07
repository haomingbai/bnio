// Stress-scale twin of
// tests/integration/io_context/local_stream_peer_close_test.cpp.
//
// Pinned contract for the local-socket split
// (docs/design/architecture/local-socket-split.md §7.2): on AF_UNIX
// SOCK_STREAM pairs (wrapped in async_io::local::stream_socket_view since
// the local-socket split), a 1 MiB write-all
// whose peer drains a quota and then closes mid-transfer must terminate with
// exactly one completion, error class EPIPE (std::generic_category,
// MSG_NOSIGNAL), partial bytes preserved, and never set_stopped — across 20
// concurrent pairs driven by 4 workers on one io_context. The success-face
// exactly-once contract at scale is pinned by ReadWriteStressTest /
// TcpStressTest; this pin closes the error-face counterpart.
//
// All waits are event-driven (completion flags / deadline-bounded yield
// loops); no sleeps synchronize the scenario.

#include <bnio/async_io/local/socket_view.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../../support/io_context/io_context_runtime_test_support.h"

#if !defined(SOCK_CLOEXEC)
#define SOCK_CLOEXEC 0
#endif

namespace {

struct completion_record {
  std::atomic<unsigned> calls{0};
  std::atomic<bool> done{false};
  std::atomic<bool> stopped{false};
  // Written once before done.store(release); read after done.load(acquire).
  std::error_code error{};
  std::size_t bytes = 0;
};

// context is null: the main thread stops the shared context after all pairs
// have terminated, mirroring ReadWriteStressTest.
struct write_contract_receiver {
  completion_record* record;

  void set_value(std::error_code ec, std::size_t size) noexcept {
    record->calls.fetch_add(1, std::memory_order_acq_rel);
    record->error = ec;
    record->bytes = size;
    record->done.store(true, std::memory_order_release);
  }

  void set_stopped() noexcept {
    record->calls.fetch_add(1, std::memory_order_acq_rel);
    record->stopped.store(true, std::memory_order_relaxed);
    record->done.store(true, std::memory_order_release);
  }
};

struct pair_state {
  int fds[2] = {-1, -1};
  completion_record record;
  std::vector<unsigned char> payload;
  std::atomic<bool> reader_ok{false};
};

// Drains exactly `quota` bytes, then closes the reader descriptor. Returns
// false when the quota could not be drained before the deadline or an
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
      (void)::close(fd);
      return false;
    }
  }
  return ::close(fd) == 0;
}

TEST(LocalWritePeerCloseStressTest,
     concurrent_write_all_peer_close_terminates_epipe_once) {
  constexpr int kPairs = 20;
  constexpr std::size_t kPayloadSize = 1U << 20;  // 1 MiB ≫ AF_UNIX sndbuf
  constexpr std::size_t kQuotaBase = 64U * 1024;  // per-pair drain quota
  constexpr unsigned kWorkers = 4;

  bnio::io_context_options options;
  options.concurrency_hint = kWorkers;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  // All pairs share one operation type: identical view/buffer/flags types.
  using sender_type = decltype(scheduler.async_write(
      std::declval<bnio::async_io::local::stream_socket_view>(),
      std::declval<decltype(bnio::buffer(
          static_cast<const unsigned char*>(nullptr), std::size_t{0}))>(),
      std::declval<int>()));
  using op_type = decltype(bexec::connect(
      std::declval<sender_type>(), std::declval<write_contract_receiver>()));

  std::vector<std::unique_ptr<pair_state>> pairs;
  std::vector<std::unique_ptr<op_type>> ops;
  pairs.reserve(static_cast<std::size_t>(kPairs));
  ops.reserve(static_cast<std::size_t>(kPairs));

  for (int i = 0; i < kPairs; ++i) {
    auto pair = std::make_unique<pair_state>();
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair->fds),
              0);
    pair->payload.resize(kPayloadSize);
    for (std::size_t j = 0; j < pair->payload.size(); ++j) {
      pair->payload[j] = static_cast<unsigned char>((i + j) & 0xff);
    }

    auto sender = scheduler.async_write(
        bnio::async_io::local::stream_socket_view(pair->fds[0]),
        bnio::buffer(static_cast<const unsigned char*>(pair->payload.data()),
                     pair->payload.size()),
        MSG_NOSIGNAL);
    // write_all_operation is non-movable: construct in place via new, as
    // read_write_stress_test.cpp does.
    ops.emplace_back(new op_type(bexec::connect(
        std::move(sender), write_contract_receiver{&pair->record})));
    bexec::start(*ops.back());
    pairs.push_back(std::move(pair));
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(8);

  // One reader per pair: drain a varying quota (< payload), then close
  // mid-transfer. The writer can only have delivered what the reader
  // drained, so the quota guarantees partial progress before the close.
  std::vector<std::thread> readers;
  readers.reserve(static_cast<std::size_t>(kPairs));
  for (int i = 0; i < kPairs; ++i) {
    pair_state& pair = *pairs[static_cast<std::size_t>(i)];
    const std::size_t quota = kQuotaBase + static_cast<std::size_t>(i) * 1024U;
    readers.emplace_back([&pair, quota, deadline] {
      pair.reader_ok.store(drain_then_close(pair.fds[1], quota, deadline),
                           std::memory_order_release);
    });
  }

  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (unsigned w = 0; w < kWorkers; ++w) {
    workers.emplace_back([&context] { context.run(); });
  }

  bool all_done = false;
  while (!all_done && std::chrono::steady_clock::now() < deadline) {
    all_done = true;
    for (const auto& pair : pairs) {
      if (!pair->record.done.load(std::memory_order_acquire)) {
        all_done = false;
        std::this_thread::yield();
        break;
      }
    }
  }
  if (!all_done) {
    ADD_FAILURE() << "timeout: not all write-all operations terminated with "
                     "EPIPE after the peer closes";
  }

  (void)context.stop();
  for (auto& w : workers) {
    w.join();
  }
  for (auto& r : readers) {
    r.join();
  }

  // Aggregate exactly-once accounting over every pair.
  for (int i = 0; i < kPairs; ++i) {
    const pair_state& pair = *pairs[static_cast<std::size_t>(i)];
    EXPECT_TRUE(pair.reader_ok.load(std::memory_order_acquire))
        << "pair " << i << ": reader failed to drain quota and close";
    EXPECT_EQ(pair.record.calls.load(std::memory_order_acquire), 1U)
        << "pair " << i << ": terminal call count";
    EXPECT_FALSE(pair.record.stopped.load(std::memory_order_acquire))
        << "pair " << i << ": completed via set_stopped";
    EXPECT_EQ(pair.record.error,
              std::error_code(EPIPE, std::generic_category()))
        << "pair " << i << ": error class";
    EXPECT_GT(pair.record.bytes, 0U)
        << "pair " << i << ": partial bytes not preserved";
    (void)::close(pair.fds[0]);
  }
}

}  // namespace
