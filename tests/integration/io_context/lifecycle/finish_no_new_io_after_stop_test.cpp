#include <bnio/bnio.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// A healthy stop returns in milliseconds.  The pre-guard defect hangs
// finish() for as long as the republishing receiver keeps feeding it, so
// a small bound keeps the red run cheap while still being orders of
// magnitude above the healthy path.
constexpr auto kStopBound = 2s;

struct op_holder_base {
  virtual ~op_holder_base() = default;
};

template <typename Op>
struct op_holder : op_holder_base {
  Op op;
  template <typename Sender, typename Receiver>
  explicit op_holder(Sender&& s, Receiver&& r)
      : op(bexec::connect(std::forward<Sender>(s), std::forward<Receiver>(r))) {
  }
};

// Heap-owned state shared with the worker thread.  Everything the parked
// worker can still reference lives here — including the worker's own
// std::thread object, whose join is owned by the stopper thread — so a
// hung run can leak the whole owner instead of destroying objects (or a
// still-joinable thread) under a live thread's feet.
struct test_owner {
  std::unique_ptr<bnio::io_context> context;
  std::unique_ptr<std::thread> worker;
  std::vector<std::unique_ptr<op_holder_base>> ops;
  int read_fd = -1;
  std::atomic<unsigned> started{0};
  std::atomic<unsigned> value_completions{0};
  std::atomic<unsigned> canceled_completions{0};
  std::atomic<unsigned> error_completions{0};
  std::atomic<unsigned> stopped_completions{0};
};

void start_poll(test_owner& owner);

// Receiver of the self-republishing poll chain.  The fd is permanently
// readable, so every real completion finds the poll immediately ready
// again and unconditionally republishes: the receiver queries no context
// state and sets no flag of its own.  A healthy stop must therefore end
// the chain through the stop channel alone — this is exactly the contract
// the consume-loop teardown guard implements.
struct republish_receiver {
  test_owner* owner = nullptr;

  void set_value(std::error_code ec, unsigned /*events*/) noexcept {
    if (ec == std::make_error_code(std::errc::operation_canceled)) {
      // Stop channel: work consumed while stopping is aborted, never run.
      owner->canceled_completions.fetch_add(1, std::memory_order_acq_rel);
      return;  // the chain ends here
    }
    if (ec) {
      // Unexpected native error (e.g. EBADF): not part of the pinned
      // contract, but still one terminal call for one started operation.
      owner->error_completions.fetch_add(1, std::memory_order_acq_rel);
      return;
    }
    owner->value_completions.fetch_add(1, std::memory_order_acq_rel);
    start_poll(*owner);
  }

  void set_stopped() noexcept {
    owner->stopped_completions.fetch_add(1, std::memory_order_acq_rel);
  }
};

void start_poll(test_owner& owner) {
  auto view = bnio::async_io::descriptor_view(owner.read_fd);
  auto sender = owner.context->get_post_scheduler().async_poll(
      view, static_cast<unsigned>(POLLIN));
  using Op = decltype(bexec::connect(sender, republish_receiver{}));
  auto* holder =
      new (std::nothrow) op_holder<Op>(sender, republish_receiver{&owner});
  if (holder == nullptr) {
    return;  // allocation failure only ends the chain early
  }
  owner.ops.emplace_back(holder);
  owner.started.fetch_add(1, std::memory_order_acq_rel);
  bexec::start(holder->op);
}

// Liveness regression for kqueue teardown (mirrors the io_uring guard).
//
// Defect (pre-guard kqueue behavior): consume_io_tasks() had no teardown
// guard, so an operation taken from the I/O queues while finish() drained
// them was really registered (EV_ADD) and executed.  A receiver that
// unconditionally republishes an immediately-ready poll after every
// completion keeps the Phase 1 loop (run_cpu_batch() ||
// consume_io_tasks()) fed forever: finish() never returns and stop()
// spins in stop_internal() waiting for the worker.  io_uring refuses new
// I/O past the stop observation (teardown guard in consume_io_tasks())
// and converges; this test pins the same contract for kqueue.
TEST(LifecycleTest, finish_stops_executing_io_published_after_stop) {
  int sv[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  auto owner = std::make_unique<test_owner>();
  owner->context = std::make_unique<bnio::io_context>();
  if (!owner->context->is_open()) {
    ::close(sv[0]);
    ::close(sv[1]);
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  owner->read_fd = sv[0];

  // One byte on the read end keeps the poll permanently ready; poll never
  // consumes data, so every republished operation is immediately ready.
  ASSERT_EQ(::write(sv[1], "x", 1), 1);

  owner->worker =
      std::make_unique<std::thread>([&owner] { (void)owner->context->run(); });
  std::this_thread::sleep_for(50ms);

  start_poll(*owner);
  // Let the republish chain spin against the live context first, so the
  // value-completion counter has real evidence the chain ran.
  std::this_thread::sleep_for(20ms);

  // Bounded stop.  On timeout the owner is intentionally leaked — the
  // parked worker keeps referencing it, and the stopper thread keeps
  // joining the worker through the owner's thread object — so nothing a
  // live thread uses is ever destroyed; process exit reaps them all.
  // The test must fail, never hang.
  std::atomic<bool> stop_done{false};
  std::thread stopper([&owner, &stop_done] {
    (void)owner->context->stop();
    owner->worker->join();
    stop_done.store(true, std::memory_order_release);
  });

  const auto deadline = std::chrono::steady_clock::now() + kStopBound;
  while (!stop_done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }

  if (!stop_done.load(std::memory_order_acquire)) {
    ADD_FAILURE() << "stop() did not return within "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         kStopBound)
                         .count()
                  << "ms: finish() kept executing republished always-ready I/O "
                     "instead of aborting it through the stop channel (missing "
                     "consume-loop teardown guard)";
    (void)owner.release();
    stopper.detach();
    return;  // fds leak with the owner; process exit reaps everything
  }
  stopper.join();

  const unsigned started = owner->started.load(std::memory_order_acquire);
  const unsigned value =
      owner->value_completions.load(std::memory_order_acquire);
  const unsigned canceled =
      owner->canceled_completions.load(std::memory_order_acquire);
  const unsigned error =
      owner->error_completions.load(std::memory_order_acquire);
  const unsigned stopped =
      owner->stopped_completions.load(std::memory_order_acquire);

  // The republish chain actually ran against the live context before the
  // stop: the value counter is evidence the scenario was exercised.
  EXPECT_GE(value, 1u);
  // The pinned contract: work consumed while the context is stopping is
  // delivered through the stop channel (operation_canceled for this
  // token-less receiver), never registered or executed.
  EXPECT_GE(canceled, 1u);
  EXPECT_EQ(error, 0u);
  // Every started operation reached exactly one terminal call: the
  // teardown dropped nothing and double-delivered nothing.
  EXPECT_EQ(started, value + canceled + error + stopped);

  ::close(sv[0]);
  ::close(sv[1]);
}

}  // namespace
