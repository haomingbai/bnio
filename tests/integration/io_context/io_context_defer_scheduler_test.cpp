#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include "../../support/io_context/io_context_loopback_test_support.h"

static_assert(!std::is_same_v<bnio::io_context::post_scheduler,
                              bnio::io_context::defer_scheduler>);
static_assert(!std::is_same_v<bnio::io_context::dispatch_scheduler,
                              bnio::io_context::defer_scheduler>);

namespace {

// Owns the inner operation state of the nested defer schedule below. The
// deferred task is referenced by the shared queue after the handler that
// starts it returns, and schedule operations are non-movable (intrusively
// queued), so the operation state cannot live on the handler's stack
// frame. The holder is owned by the outer receiver, which lives inside the
// outer operation state on the test frame and outlives run().
struct defer_inline_inner_holder {
  defer_inline_inner_holder(std::shared_ptr<schedule_state> state,
                            bnio::io_context* context)
      : operation(
            bexec::connect(bexec::schedule(context->get_defer_scheduler()),
                           schedule_receiver{std::move(state), context, 42})) {}

  decltype(bexec::connect(
      std::declval<bnio::io_context::defer_scheduler&>().schedule(),
      std::declval<schedule_receiver>())) operation;
};

// Runs on a context thread (reached through a post schedule) and starts a
// defer schedule from there. Mirrors dispatch_inline_outer_receiver, but
// the inner operation must be heap-held and must not stop the context
// before the deferred inner task completes.
struct defer_inline_outer_receiver {
  std::shared_ptr<schedule_state> state;
  bnio::io_context* context = nullptr;
  std::shared_ptr<defer_inline_inner_holder> inner;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      (void)context->stop();
      return;
    }
    inner = std::make_shared<defer_inline_inner_holder>(state, context);
    bexec::start(inner->operation);
    // Contract: even when started from a context thread, a defer schedule
    // must not complete during start() — dispatch does, defer must not.
    state->completed_during_start = state->signal == signal_kind::value;
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    (void)context->stop();
  }
};

// Shared state for the worker-local-queue escape test. Cross-thread
// accesses are ordered by: the outer handler writes outer_tid before it
// spins; the inner handler writes inner_tid before the release store into
// inner_done; the outer spin's acquire load of inner_done and the worker
// joins on the main thread publish everything the assertions read.
struct defer_escape_state {
  std::atomic<bool> inner_done{false};
  std::thread::id outer_tid{};
  std::thread::id inner_tid{};
  signal_kind outer_signal = signal_kind::none;
  signal_kind inner_signal = signal_kind::none;
};

struct defer_escape_inner_receiver {
  defer_escape_state* escape = nullptr;

  void set_value(std::error_code ec) noexcept {
    escape->inner_signal = ec ? signal_kind::error : signal_kind::value;
    escape->inner_tid = std::this_thread::get_id();
    escape->inner_done.store(true, std::memory_order_release);
  }

  void set_stopped() noexcept {
    escape->inner_signal = signal_kind::stopped;
    escape->inner_done.store(true, std::memory_order_release);
  }
};

// Same non-movable, queue-referenced-after-start reasoning as
// defer_inline_inner_holder: the inner operation state is heap-held and
// owned by the outer receiver.
struct defer_escape_inner_holder {
  defer_escape_inner_holder(defer_escape_state* escape,
                            bnio::io_context* context)
      : operation(
            bexec::connect(bexec::schedule(context->get_defer_scheduler()),
                           defer_escape_inner_receiver{escape})) {}

  decltype(bexec::connect(
      std::declval<bnio::io_context::defer_scheduler&>().schedule(),
      std::declval<defer_escape_inner_receiver>())) operation;
};

struct defer_escape_outer_receiver {
  defer_escape_state* escape = nullptr;
  bnio::io_context* context = nullptr;
  std::shared_ptr<defer_escape_inner_holder> inner;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      escape->outer_signal = signal_kind::error;
      (void)context->stop();
      return;
    }
    escape->outer_signal = signal_kind::value;
    escape->outer_tid = std::this_thread::get_id();

    inner = std::make_shared<defer_escape_inner_holder>(escape, context);
    bexec::start(inner->operation);

    // Spin until the inner defer task ran. Failure mode: if defer degraded
    // to the worker-local queue fast path, the inner task would sit on
    // THIS worker's local queue while this handler spins without draining
    // it — both workers deadlock and the ctest TIMEOUT fires.
    while (!escape->inner_done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    (void)context->stop();
  }

  void set_stopped() noexcept {
    escape->outer_signal = signal_kind::stopped;
    (void)context->stop();
  }
};

// Receiver for a schedule() started after io_context::stop(): classifies
// the terminal call so the test can assert the inline-canceled contract.
struct defer_after_stop_receiver {
  std::atomic<int>* completed = nullptr;
  std::atomic<int>* canceled = nullptr;
  std::atomic<int>* stopped = nullptr;

  void set_value(std::error_code ec) noexcept {
    completed->fetch_add(1, std::memory_order_acq_rel);
    if (ec == std::make_error_code(std::errc::operation_canceled)) {
      canceled->fetch_add(1, std::memory_order_acq_rel);
    }
  }

  void set_stopped() noexcept {
    stopped->fetch_add(1, std::memory_order_acq_rel);
  }
};

// Accept/connect/byte receivers for the echo smoke test. Identical to the
// stock receivers except that the completion counter is atomic: with two
// run() workers, both phase completions may run concurrently.
struct defer_echo_accept_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;
  std::atomic<unsigned>* completions = nullptr;
  unsigned target = 1;

  void set_value(std::error_code ec, bnio::tcp_socket socket) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->fd = socket.release();
    }
    finish();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    finish();
  }

 private:
  void finish() noexcept {
    if (completions->fetch_add(1, std::memory_order_acq_rel) + 1 == target) {
      (void)context->stop();
    }
  }
};

struct defer_echo_connect_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;
  std::atomic<unsigned>* completions = nullptr;
  unsigned target = 1;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
    }
    finish();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    finish();
  }

 private:
  void finish() noexcept {
    if (completions->fetch_add(1, std::memory_order_acq_rel) + 1 == target) {
      (void)context->stop();
    }
  }
};

struct defer_echo_byte_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;
  std::atomic<unsigned>* completions = nullptr;
  unsigned target = 1;

  void set_value(std::error_code ec, std::size_t size) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->size = size;
    }
    finish();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    finish();
  }

 private:
  void finish() noexcept {
    if (completions->fetch_add(1, std::memory_order_acq_rel) + 1 == target) {
      (void)context->stop();
    }
  }
};

TEST(IoContextDeferSchedulerTest,
     defer_scheduler_schedule_never_completes_inline) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto state = std::make_shared<schedule_state>();
  state->order.reserve(1);

  auto operation =
      bexec::connect(bexec::schedule(context.get_post_scheduler()),
                     defer_inline_outer_receiver{state, &context, nullptr});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_FALSE(state->completed_during_start);
  EXPECT_EQ(state->order.size(), 1);
  EXPECT_EQ(state->order[0], 42);
}

TEST(IoContextDeferSchedulerTest,
     defer_scheduler_schedule_posts_from_foreign_thread) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto state = std::make_shared<schedule_state>();
  state->order.reserve(1);

  schedule_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.value = 99;

  auto operation = bexec::connect(
      bexec::schedule(context.get_defer_scheduler()), std::move(receiver));
  bexec::start(operation);

  std::thread runner([&context] { context.run(); });
  runner.join();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->order.size(), 1);
  EXPECT_EQ(state->order[0], 99);
}

TEST(IoContextDeferSchedulerTest,
     defer_scheduler_schedule_pre_stopped_token_stops) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  auto state = std::make_shared<schedule_state>();
  stopped_schedule_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};

  auto operation = bexec::connect(
      bexec::schedule(context.get_defer_scheduler()), std::move(receiver));
  bexec::start(operation);
  context.run();

  // Contract: a stop token already canceled at start() is observed by the
  // schedule operation and completes via set_stopped.
  EXPECT_EQ(state->signal, signal_kind::stopped);
}

TEST(IoContextDeferSchedulerTest,
     defer_schedule_after_stop_completes_canceled) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  // Gate: confirm a worker is inside run() before stopping, so the test
  // exercises a stop against a live context.
  std::atomic<bool> worker_active{false};
  struct active_flag_receiver {
    std::atomic<bool>* flag;
    void set_value(std::error_code) noexcept {
      flag->store(true, std::memory_order_release);
    }
    void set_stopped() noexcept {
      flag->store(true, std::memory_order_release);
    }
  };
  auto active_operation =
      bexec::connect(bexec::schedule(context.get_post_scheduler()),
                     active_flag_receiver{&worker_active});
  bexec::start(active_operation);

  std::thread worker([&context] { context.run(); });
  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker_active.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        (void)context.stop();
        worker.join();
        FAIL() << "timed out waiting for the worker to become active";
      }
      std::this_thread::yield();
    }
  }
  (void)context.stop();
  worker.join();

  // Contract: a defer schedule started after stop() has its publish
  // rejected, so it completes inline via set_value(operation_canceled) and
  // never strands.
  std::atomic<int> completed{0};
  std::atomic<int> canceled{0};
  std::atomic<int> stopped{0};
  auto operation = bexec::connect(
      bexec::schedule(context.get_defer_scheduler()),
      defer_after_stop_receiver{&completed, &canceled, &stopped});
  bexec::start(operation);

  // Bounded wait: a stranded operation would never complete.
  for (int i = 0; i < 100 && completed.load(std::memory_order_acquire) == 0;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(completed.load(std::memory_order_acquire), 1);
  EXPECT_EQ(canceled.load(std::memory_order_acquire), 1);
  EXPECT_EQ(stopped.load(std::memory_order_acquire), 0);
}

// Core regression guard for the defer scheduling policy: a defer task
// started from inside a worker's handler must be drained by another
// worker. With two run() workers, the outer handler starts a nested defer
// schedule and then spins; only a different worker can complete it. If
// defer degraded to the worker-local queue fast path, the inner task would
// stay on the spinning worker's local queue and both workers would
// deadlock until the ctest TIMEOUT fires.
TEST(IoContextDeferSchedulerTest, defer_task_escapes_worker_local_queue) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  defer_escape_state escape;

  auto operation =
      bexec::connect(bexec::schedule(context.get_defer_scheduler()),
                     defer_escape_outer_receiver{&escape, &context, nullptr});
  bexec::start(operation);

  std::thread first_worker([&context] { context.run(); });
  std::thread second_worker([&context] { context.run(); });
  first_worker.join();
  second_worker.join();

  EXPECT_EQ(escape.outer_signal, signal_kind::value);
  EXPECT_EQ(escape.inner_signal, signal_kind::value);
  // The spinning worker cannot drain the shared queue, so the inner task
  // must have run on the other worker; an inline completion during start()
  // would record the outer thread's id here too.
  EXPECT_NE(escape.outer_tid, escape.inner_tid);
}

// End-to-end smoke: accept through the defer scheduler (publish_io_deferred
// shared-queue path exercised by two workers), then a 64-byte echo over the
// accepted connection. Each phase runs on a fresh context with two run()
// workers, mirroring the multi-round pattern of the read/write tests.
TEST(IoContextDeferSchedulerTest, defer_accept_echo_smoke) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::tcp_acceptor acceptor;
  EXPECT_FALSE(acceptor.open(bnio::ip::tcp::v4()));
  EXPECT_FALSE(acceptor.set_reuse_address(true));
  EXPECT_FALSE(acceptor.bind(bnio::ip::endpoint::loopback_v4(0)));
  EXPECT_FALSE(acceptor.listen(4));
  const bnio::ip::endpoint endpoint = bound_loopback_endpoint(acceptor);

  bnio::tcp_socket client;
  EXPECT_FALSE(client.open(bnio::ip::tcp::v4()));

  // Phase 1: accept via the defer scheduler, connect via the post
  // scheduler, both drained by two run() workers.
  std::atomic<unsigned> setup_completions{0};

  defer_echo_accept_receiver accept_receiver;
  accept_receiver.context = &context;
  accept_receiver.completions = &setup_completions;
  accept_receiver.target = 2;
  auto accept_state = accept_receiver.state;

  defer_echo_connect_receiver connect_receiver;
  connect_receiver.context = &context;
  connect_receiver.completions = &setup_completions;
  connect_receiver.target = 2;
  auto connect_state = connect_receiver.state;

  auto accept_operation = bexec::connect(
      acceptor.async_accept(context.get_defer_scheduler(), SOCK_CLOEXEC),
      std::move(accept_receiver));
  auto connect_operation = bexec::connect(
      client.async_connect(context.get_post_scheduler(), endpoint),
      std::move(connect_receiver));

  bexec::start(accept_operation);
  bexec::start(connect_operation);

  std::thread first_worker([&context] { context.run(); });
  std::thread second_worker([&context] { context.run(); });
  first_worker.join();
  second_worker.join();

  EXPECT_EQ(setup_completions.load(std::memory_order_acquire), 2);
  EXPECT_EQ(accept_state->signal, signal_kind::value);
  ASSERT_TRUE(accept_state->fd >= 0);
  EXPECT_TRUE((::fcntl(accept_state->fd, F_GETFL, 0) & O_NONBLOCK) != 0);
  EXPECT_EQ(connect_state->signal, signal_kind::value);

  constexpr std::string_view payload =
      "0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef";

  // Phase 2: client writes the payload, server reads it (post scheduler,
  // fresh context, two workers).
  bnio::io_context echo_context;
  if (!context_available(echo_context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  bnio::tcp_socket server_socket(accept_state->fd);
  accept_state->fd = -1;  // ownership moved into server_socket
  auto post_scheduler = echo_context.get_post_scheduler();

  std::array<char, 64> server_received{};
  std::atomic<unsigned> first_hop_completions{0};

  defer_echo_byte_receiver server_read_receiver;
  server_read_receiver.context = &echo_context;
  server_read_receiver.completions = &first_hop_completions;
  server_read_receiver.target = 2;
  auto server_read_state = server_read_receiver.state;

  defer_echo_byte_receiver client_write_receiver;
  client_write_receiver.context = &echo_context;
  client_write_receiver.completions = &first_hop_completions;
  client_write_receiver.target = 2;
  auto client_write_state = client_write_receiver.state;

  auto server_read_operation = bexec::connect(
      server_socket.async_read(post_scheduler, bnio::buffer(server_received)),
      std::move(server_read_receiver));
  auto client_write_operation = bexec::connect(
      client.async_write(post_scheduler, bnio::buffer(payload), MSG_NOSIGNAL),
      std::move(client_write_receiver));

  bexec::start(server_read_operation);
  bexec::start(client_write_operation);

  std::thread third_worker([&echo_context] { echo_context.run(); });
  std::thread fourth_worker([&echo_context] { echo_context.run(); });
  third_worker.join();
  fourth_worker.join();

  ASSERT_EQ(first_hop_completions.load(std::memory_order_acquire), 2);
  ASSERT_EQ(server_read_state->signal, signal_kind::value);
  ASSERT_EQ(server_read_state->size, payload.size());
  ASSERT_TRUE(
      std::memcmp(server_received.data(), payload.data(), payload.size()) == 0);
  ASSERT_EQ(client_write_state->signal, signal_kind::value);
  ASSERT_EQ(client_write_state->size, payload.size());

  // Phase 3: server echoes the payload back, client reads it (post
  // scheduler, fresh context, two workers).
  bnio::io_context echo_back_context;
  if (!context_available(echo_back_context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto echo_back_scheduler = echo_back_context.get_post_scheduler();

  std::array<char, 64> client_received{};
  std::atomic<unsigned> second_hop_completions{0};

  defer_echo_byte_receiver server_write_receiver;
  server_write_receiver.context = &echo_back_context;
  server_write_receiver.completions = &second_hop_completions;
  server_write_receiver.target = 2;
  auto server_write_state = server_write_receiver.state;

  defer_echo_byte_receiver client_read_receiver;
  client_read_receiver.context = &echo_back_context;
  client_read_receiver.completions = &second_hop_completions;
  client_read_receiver.target = 2;
  auto client_read_state = client_read_receiver.state;

  auto server_write_operation = bexec::connect(
      server_socket.async_write(
          echo_back_scheduler,
          bnio::buffer(server_received.data(), server_read_state->size),
          MSG_NOSIGNAL),
      std::move(server_write_receiver));
  auto client_read_operation = bexec::connect(
      client.async_read(echo_back_scheduler, bnio::buffer(client_received)),
      std::move(client_read_receiver));

  bexec::start(server_write_operation);
  bexec::start(client_read_operation);

  std::thread fifth_worker([&echo_back_context] { echo_back_context.run(); });
  std::thread sixth_worker([&echo_back_context] { echo_back_context.run(); });
  fifth_worker.join();
  sixth_worker.join();

  EXPECT_EQ(second_hop_completions.load(std::memory_order_acquire), 2);
  EXPECT_EQ(server_write_state->signal, signal_kind::value);
  EXPECT_EQ(client_read_state->signal, signal_kind::value);
  EXPECT_EQ(client_read_state->size, payload.size());
  EXPECT_TRUE(
      std::memcmp(client_received.data(), payload.data(), payload.size()) == 0);
}

// Socketpair helper for the defer eager-retention tests. Same pattern as
// io_context_eager_optional_test.cpp.
[[nodiscard]] std::array<int, 2> make_socketpair() {
  int sockets[2] = {-1, -1};
  const int rc = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets);
  EXPECT_EQ(rc, 0);
  return {sockets[0], sockets[1]};
}

// Regression guard for the defer I/O policy: with the eager runtime switch
// ON, a defer-kind read still attempts the eager immediate probe — the
// schedule policy keeps k_immediate enabled for every current kind. The
// observation point is the socket receive queue right after start(),
// before run(): the probe consumes the pending bytes synchronously on the
// starting thread and publishes the completion through the shared CPU
// queue, so the kernel buffer is already empty when the peek runs.
TEST(IoContextDeferSchedulerTest,
     defer_io_read_completes_eagerly_with_eager_on) {
  bnio::io_context_options options;
  options.enable_immediate_io = true;  // eager runtime switch explicitly on
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto sockets = make_socketpair();
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  constexpr std::string_view payload = "defer-eager";
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));

  std::array<char, 32> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto operation =
      bexec::connect(context.get_defer_scheduler().async_read_some(
                         receiver_socket.view(), bnio::buffer(bytes), 0),
                     std::move(receiver));
  bexec::start(operation);

  // The eager probe consumed the data during start(), before run().
  std::array<char, 32> peek{};
  const ssize_t peeked = ::recv(receiver_socket.native_handle(), peek.data(),
                                peek.size(), MSG_PEEK | MSG_DONTWAIT);
  EXPECT_EQ(peeked, -1);
  EXPECT_EQ(errno, EAGAIN);

  context.run();

  // The completion is delivered through the shared CPU queue with the real
  // payload.
  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

// Write-side twin of the read guard: with the eager runtime switch ON, a
// defer-kind write pushes the payload into the peer's receive queue during
// start() — the eager probe runs on the starting thread for every kind
// that keeps k_immediate enabled.
TEST(IoContextDeferSchedulerTest,
     defer_io_write_completes_eagerly_with_eager_on) {
  bnio::io_context_options options;
  options.enable_immediate_io = true;  // eager runtime switch explicitly on
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto sockets = make_socketpair();
  bnio::tcp_socket sender_socket(sockets[0]);
  bnio::tcp_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "defer-eager";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto operation = bexec::connect(
      context.get_defer_scheduler().async_write(
          sender_socket.view(), bnio::buffer(payload), MSG_NOSIGNAL),
      std::move(receiver));
  bexec::start(operation);

  // The eager probe pushed the bytes into the peer's receive queue during
  // start().
  std::array<char, 32> peek{};
  const ssize_t peeked = ::recv(receiver_socket.native_handle(), peek.data(),
                                peek.size(), MSG_PEEK | MSG_DONTWAIT);
  EXPECT_EQ(peeked, static_cast<ssize_t>(payload.size()));

  context.run();

  // The completion is delivered through the shared CPU queue and the peer
  // receives the payload.
  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  std::array<char, 32> got{};
  EXPECT_EQ(::recv(receiver_socket.native_handle(), got.data(), got.size(), 0),
            static_cast<ssize_t>(payload.size()));
  EXPECT_TRUE(std::memcmp(got.data(), payload.data(), payload.size()) == 0);
}

}  // namespace
