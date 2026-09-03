// Regression tests for the SQ-full remainder of a prepared I/O batch.
//
// Contract under test (io_uring_context::consume_io_tasks()):
// prepare_io_batch() fills SQEs until io_uring_get_sqe() reports the SQ is
// full, then hands the operations that did not fit back as the "remainder".
// Those operations never reached an SQE and never took part in the submit
// call, so they must never inherit an errno produced by that submit: they
// are parked in the run-loop retry slot (pending_io_retry_) and prepared
// again on the next pass, exactly like a same-batch operation that stayed
// behind in the source queue because the take was short-circuited.
//
// The ring is created with entries == 2 so that a batch of 8 polls can never
// be prepared in a single pass — the remainder is guaranteed to be non-empty.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bnio_async_io_io_uring_test;

using bnio::async_io::linux_native::io_uring_poll_sender_operation;

// 8 operations against an SQ that holds 2: 6 of them are the remainder.
constexpr unsigned k_operation_count = 8;

// A healthy run loop reacts within milliseconds; only a regression (a stuck
// retry slot or a livelock) can exceed this bound.
constexpr auto k_sync_bound = std::chrono::seconds(5);

// Polls predicate() every 200us until it holds or the bound elapses.
template <typename Predicate>
bool wait_until(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + k_sync_bound;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

// One counter per completion channel. The project's test discipline forbids
// merging set_value(ec) and set_stopped() into a single counter: a merged
// count cannot distinguish a delivered error from a cancelled operation, and
// -EINTR is separated out further so an errno inherited from a submit the
// operation never took part in is visible as such.
struct channel_counters {
  std::atomic<unsigned> value{0};
  std::atomic<unsigned> eintr{0};
  std::atomic<unsigned> other_error{0};
  std::atomic<unsigned> stopped{0};

  [[nodiscard]] unsigned total() const noexcept {
    return value.load(std::memory_order_acquire) +
           eintr.load(std::memory_order_acquire) +
           other_error.load(std::memory_order_acquire) +
           stopped.load(std::memory_order_acquire);
  }
};

struct sq_full_receiver {
  std::shared_ptr<channel_counters> counters;
  io_uring_context* context = nullptr;

  void set_value(std::error_code ec, unsigned events) noexcept {
    if (!ec) {
      EXPECT_EQ(events, static_cast<unsigned>(POLLIN));
      counters->value.fetch_add(1, std::memory_order_acq_rel);
    } else if (ec == std::error_code(EINTR, std::generic_category())) {
      counters->eintr.fetch_add(1, std::memory_order_acq_rel);
    } else {
      counters->other_error.fetch_add(1, std::memory_order_acq_rel);
    }
    finish_if_last();
  }

  void set_stopped() noexcept {
    counters->stopped.fetch_add(1, std::memory_order_acq_rel);
    finish_if_last();
  }

 private:
  // Only the last completion stops the context, so every operation is
  // observable through its own channel counter.
  void finish_if_last() noexcept {
    if (counters->total() == k_operation_count && context != nullptr) {
      (void)context->stop();
    }
  }
};

using poll_operation = io_uring_poll_sender_operation<sq_full_receiver>;

/** RAII pipe pair; both ends are close-on-exec. */
class pipe_pair {
 public:
  pipe_pair() { ::pipe2(fds_.data(), O_CLOEXEC); }

  ~pipe_pair() {
    for (int fd : fds_) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }

  pipe_pair(const pipe_pair&) = delete;
  pipe_pair& operator=(const pipe_pair&) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return fds_[0] >= 0 && fds_[1] >= 0;
  }

  [[nodiscard]] int read_fd() const noexcept { return fds_[0]; }
  [[nodiscard]] int write_fd() const noexcept { return fds_[1]; }

  /** Makes the read end readable so an armed POLLIN completes. */
  void make_readable() noexcept { (void)::write(fds_[1], "x", 1); }

 private:
  std::array<int, 2> fds_{-1, -1};
};

/** Builds k_operation_count pipes and the matching poll operations. */
class poll_fixture {
 public:
  explicit poll_fixture(io_uring_context& context) {
    for (unsigned i = 0; i < k_operation_count; ++i) {
      pipes_.push_back(std::make_unique<pipe_pair>());
      if (!pipes_.back()->valid()) {
        // ASSERT_* is illegal inside a constructor; record the failure and
        // let the test body assert on it instead.
        valid_ = false;
      }
      operations_.push_back(std::make_unique<poll_operation>(
          context, descriptor_view(pipes_.back()->read_fd()),
          static_cast<unsigned>(POLLIN),
          sq_full_receiver{counters_, &context}));
    }
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }

  [[nodiscard]] const std::shared_ptr<channel_counters>& counters()
      const noexcept {
    return counters_;
  }

  /** Publishes every operation, i.e. queues them all for one take. */
  void start_all() noexcept {
    for (auto& operation : operations_) {
      bexec::start(*operation);
    }
  }

  pipe_pair& pipe_at(std::size_t index) noexcept { return *pipes_[index]; }

 private:
  std::shared_ptr<channel_counters> counters_ =
      std::make_shared<channel_counters>();
  bool valid_ = true;
  std::vector<std::unique_ptr<pipe_pair>> pipes_;
  std::vector<std::unique_ptr<poll_operation>> operations_;
};

// The remainder of a full SQ must be retried on the next run-loop pass and
// delivered through set_value({}, mask). No operation may surface EAGAIN or
// EINTR: neither errno was produced by anything the operation did.
TEST(IoUringSqFullSubmitFailureTest,
     sq_full_remainder_is_retried_and_delivered) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  io_uring_context_options options;
  options.entries = 2;
  if (!queue_init_shared_or_skip(context, global_tasks, options)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  poll_fixture fixture(context);
  ASSERT_TRUE(fixture.valid());
  const auto& counters = fixture.counters();

  // Publish before the worker starts so the first take sees all 8: with an
  // SQ of 2 only 2 fit, and the other 6 are the remainder under test.
  fixture.start_all();

  std::thread worker([&context] { context.run(); });

  // Parked means every published poll has been prepared and submitted —
  // i.e. the remainder has already gone through the retry slot.
  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked with the polls inflight";

  // Make the pipes readable one at a time and reap each completion: the CQ
  // ring only has entries*2 == 4 slots, so a burst of 8 CQEs would overflow
  // it and turn this into a CQ-overflow test instead.
  for (unsigned i = 0; i < k_operation_count; ++i) {
    fixture.pipe_at(i).make_readable();
    ASSERT_TRUE(wait_until([&counters, i] {
      return counters->value.load() == i + 1;
    })) << "poll "
        << i << " never completed with the value channel";
  }

  worker.join();

  EXPECT_EQ(counters->value.load(), k_operation_count);
  EXPECT_EQ(counters->eintr.load(), 0U);
  EXPECT_EQ(counters->other_error.load(), 0U);
  EXPECT_EQ(counters->stopped.load(), 0U);
}

// Installs a no-op SIGUSR1 handler (without SA_RESTART) exactly once.
// SA_RESTART must stay off: with it set, io_uring_enter is restarted
// automatically and never returns -EINTR.
void install_noop_sigusr1_handler() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction action;
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)::sigaction(SIGUSR1, &action, nullptr);
  });
}

// A submit failure must only fail the operations that took part in it. The
// remainder never reached an SQE, so it must be retried — never handed the
// errno of a submit it had nothing to do with. Observationally: a single
// failed submit fails at most the prepared head of the batch, never the
// whole batch, so -EINTR can never be the outcome of every operation.
//
// Note on reachability: io_uring_submit() only reaches the wait path (and
// hence -EINTR, which io_uring_enter(2) documents as "can happen while
// waiting for events with IORING_ENTER_GETEVENTS") when liburing adds
// IORING_ENTER_GETEVENTS — that happens when the ring reports CQ overflow or
// pending task work. A 2-entry SQ with an 8-slot CQ makes that likely, but
// it is not guaranteed, so this case is a guard rail: it asserts the
// partition invariant and, when a -EINTR does surface, that it never
// covers the whole batch.
TEST(IoUringSqFullSubmitFailureTest,
     submit_failure_does_not_fail_never_prepared_remaining) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  io_uring_context_options options;
  options.entries = 2;
  if (!queue_init_shared_or_skip(context, global_tasks, options)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }
  install_noop_sigusr1_handler();

  poll_fixture fixture(context);
  ASSERT_TRUE(fixture.valid());
  const auto& counters = fixture.counters();

  // The pipes are readable from the start: every poll completes as soon as
  // it is submitted, so all 8 operations terminate and none is stranded.
  for (unsigned i = 0; i < k_operation_count; ++i) {
    fixture.pipe_at(i).make_readable();
  }

  pthread_t worker_tid{};
  std::atomic<bool> tid_ready{false};
  std::thread worker([&context, &worker_tid, &tid_ready] {
    worker_tid = pthread_self();
    tid_ready.store(true, std::memory_order_release);
    context.run();
  });
  ASSERT_TRUE(wait_until([&tid_ready] { return tid_ready.load(); }));
  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked";

  // Hammer the run-loop thread with signals while it submits: without
  // SA_RESTART the interrupted io_uring_enter returns -EINTR.
  std::atomic<bool> storm_on{true};
  std::thread signaler([&storm_on, worker_tid] {
    while (storm_on.load(std::memory_order_acquire)) {
      (void)::pthread_kill(worker_tid, SIGUSR1);
    }
  });

  fixture.start_all();
  (void)wait_until(
      [&counters] { return counters->total() == k_operation_count; });
  storm_on.store(false, std::memory_order_release);
  signaler.join();

  // With the storm stopped the retries must all land; a stranded operation
  // (or a retry slot that spins without progress) fails this bound.
  ASSERT_TRUE(wait_until([&counters] {
    return counters->total() == k_operation_count;
  })) << "operations were stranded: value="
      << counters->value.load() << " eintr=" << counters->eintr.load()
      << " other_error=" << counters->other_error.load()
      << " stopped=" << counters->stopped.load();

  worker.join();

  EXPECT_EQ(counters->total(), k_operation_count);
  EXPECT_EQ(counters->stopped.load(), 0U);
  // The defect assertion: one failed submit may only fail the operations
  // it actually submitted, so -EINTR can never be every operation's
  // outcome. Under the old code a single -EINTR took down the prepared
  // head and the never-prepared remainder together.
  EXPECT_LT(counters->eintr.load(), k_operation_count)
      << "the SQ-full remainder inherited an errno from a submit it never "
         "took part in";
  EXPECT_EQ(counters->value.load() + counters->eintr.load() +
                counters->other_error.load() + counters->stopped.load(),
            k_operation_count);
}

}  // namespace
