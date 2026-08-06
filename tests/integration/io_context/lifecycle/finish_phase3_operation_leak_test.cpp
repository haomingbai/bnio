#include <bnio/bnio.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace {

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

// Receiver for the second (phantom) async_read_some.
// Counts how many times it was completed — if the count was 0, the
// Phase 3 leak would have been confirmed (the operation was never
// consumed by any run-loop phase).
struct second_receiver {
  std::atomic<int>* counter = nullptr;

  void set_value(std::error_code /*ec*/, std::size_t /*n*/) noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
  }

  void set_stopped() noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
  }
};

// Receiver for the first async_read_some that will be aborted by stop().
// In set_stopped(), it starts a second async_read_some on the same fd.
struct first_receiver {
  bnio::io_context* ctx = nullptr;
  int fd = -1;
  std::atomic<int>* first_counter = nullptr;
  std::atomic<int>* second_counter = nullptr;
  char* second_buf = nullptr;
  std::size_t second_buf_size = 0;
  std::vector<std::unique_ptr<op_holder_base>>* ops = nullptr;

  void set_value(std::error_code /*ec*/, std::size_t /*n*/) noexcept {
    if (first_counter) first_counter->fetch_add(1, std::memory_order_relaxed);
  }

  void set_stopped() noexcept {
    if (first_counter) first_counter->fetch_add(1, std::memory_order_relaxed);

    // Phase 3 bug repro: start a new I/O operation from within
    // set_stopped(). When this is called from finish() Phase 3
    // (via abort_inflight_io -> push_cpu -> execute_tasks), the new
    // operation goes to local_state_.io via the worker-local fast
    // path in publish_io(). Phase 3 never calls consume_io_tasks(),
    // so this operation is never consumed.
    auto sched = ctx->get_post_scheduler();
    auto view = bnio::async_io::stream_socket_view(fd);
    auto sender =
        sched.async_read_some(view, bnio::buffer(second_buf, second_buf_size));

    using Op = decltype(bexec::connect(sender, second_receiver{}));
    auto holder = std::make_unique<op_holder<Op>>(
        sender, second_receiver{second_counter});
    bexec::start(holder->op);
    if (ops) ops->push_back(std::move(holder));
  }
};

TEST(LifecycleTest, finish_phase3_operation_leak) {
  int sv[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    GTEST_SKIP() << "socketpair failed";
  }

  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    ::close(sv[0]);
    ::close(sv[1]);
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::atomic<int> first_completed{0};
  std::atomic<int> second_completed{0};
  char second_buf[64] = {};
  std::vector<std::unique_ptr<op_holder_base>> ops;

  std::thread worker([&ctx]() { ctx->run(); });

  // Give the worker time to initialize.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Submit the first async_read_some on the read end (sv[0]).
  // There is no data to read, so it registers with kqueue and waits.
  char first_buf[64];
  auto first_sched = ctx->get_post_scheduler();
  auto first_view = bnio::async_io::stream_socket_view(sv[0]);
  auto first_sender = first_sched.async_read_some(
      first_view, bnio::buffer(first_buf, sizeof(first_buf)));

  using FirstOp = decltype(bexec::connect(first_sender, first_receiver{}));
  auto first_holder = std::make_unique<op_holder<FirstOp>>(
      first_sender,
      first_receiver{ctx.get(), sv[0], &first_completed, &second_completed,
                     second_buf, sizeof(second_buf), &ops});
  bexec::start(first_holder->op);
  ops.push_back(std::move(first_holder));

  // Give the worker time to register the read with kqueue.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Stop the context — triggers Phase 2 (abort_inflight_io) ->
  // Phase 3 (execute CPU tasks -> first_receiver::set_stopped).
  ctx->stop();

  worker.join();

  int first = first_completed.load(std::memory_order_relaxed);
  int second = second_completed.load(std::memory_order_relaxed);

  EXPECT_EQ(first, 1);
  // second receiver must receive at least one completion signal.
  EXPECT_GT(second, 0);

  ::close(sv[0]);
  ::close(sv[1]);
}

}  // namespace
