#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <thread>

#include "../../support/io_context/io_context_runtime_test_support.h"

namespace {

struct pair_byte_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 1;

  void set_value(std::error_code ec, std::size_t size) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->size = size;
    }
    complete();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    complete();
  }

 private:
  void complete() noexcept {
    if (completions != nullptr) {
      ++*completions;
      if (*completions != target) {
        return;
      }
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct deferred_stop_byte_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 1;

  void set_value(std::error_code ec, std::size_t size) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->size = size;
    }
    complete();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    complete();
  }

 private:
  void complete() noexcept {
    if (completions != nullptr) {
      ++*completions;
      if (*completions != target) {
        return;
      }
    }
    if (context != nullptr) {
      // Post stop as a separate schedule task to avoid stop() cancellation
      // issues with repeat_until chains inside read-all / write-all senders.
      auto s = context->get_post_scheduler();
      struct stop_recv {
        bnio::io_context* c;
        void set_value(std::error_code) noexcept {
          if (c) c->stop();
        }
        void set_stopped() noexcept {}
      };
      auto op = bexec::connect(s.schedule(), stop_recv{context});
      bexec::start(op);
    }
  }
};

struct stopped_byte_receiver : byte_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

TEST(IoContextReadWriteTest, ready_socket_read_completes_without_queue) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  constexpr std::string_view payload = "ready";
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(IoContextReadWriteTest, passive_drain_reads_io) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  constexpr std::string_view payload = "passive";
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));

  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(IoContextReadWriteTest, ready_socket_write_completes_without_queue) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket sender_socket(sockets[0]);
  bnio::tcp_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "ready write";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      sender_socket.async_write(scheduler, bnio::buffer(payload), MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());

  std::array<char, 32> bytes{};
  EXPECT_EQ(
      ::recv(receiver_socket.native_handle(), bytes.data(), bytes.size(), 0),
      static_cast<ssize_t>(payload.size()));
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(IoContextReadWriteTest, blocked_socket_write_falls_back_to_queue) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket sender_socket(sockets[0]);
  bnio::tcp_socket receiver_socket(sockets[1]);

  const int sender_flags = ::fcntl(sender_socket.native_handle(), F_GETFL, 0);
  EXPECT_TRUE(sender_flags >= 0);
  EXPECT_EQ(::fcntl(sender_socket.native_handle(), F_SETFL,
                    sender_flags | O_NONBLOCK),
            0);

  std::array<char, 4096> filler{};
  while (true) {
    const ssize_t result = ::send(sender_socket.native_handle(), filler.data(),
                                  filler.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (result > 0) {
      continue;
    }
    EXPECT_LT(result, 0);
    if (errno == EINTR) {
      continue;
    }
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    break;
  }

  constexpr std::string_view payload = "fallback write";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;
  auto sender = sender_socket.async_write_some(scheduler, bnio::buffer(payload),
                                               MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  std::array<char, 65536> drain{};
  while (true) {
    const ssize_t result = ::recv(receiver_socket.native_handle(), drain.data(),
                                  drain.size(), MSG_DONTWAIT);
    if (result > 0) {
      continue;
    }
    EXPECT_LT(result, 0);
    if (errno == EINTR) {
      continue;
    }
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    break;
  }

  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  std::array<char, 32> bytes{};
  EXPECT_EQ(
      ::recv(receiver_socket.native_handle(), bytes.data(), bytes.size(), 0),
      static_cast<ssize_t>(payload.size()));
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(IoContextReadWriteTest, io_idle_drain_reads) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  constexpr std::string_view payload = "auto";
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));

  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(IoContextReadWriteTest, io_idle_drain_read_write_pair) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  constexpr std::string_view payload = "auto pair";
  std::array<char, 32> bytes{};
  unsigned completions = 0;

  pair_byte_receiver read_receiver;
  read_receiver.context = &context;
  read_receiver.completions = &completions;
  read_receiver.target = 2;
  auto read_state = read_receiver.state;

  pair_byte_receiver write_receiver;
  write_receiver.context = &context;
  write_receiver.completions = &completions;
  write_receiver.target = 2;
  auto write_state = write_receiver.state;

  auto read_sender = receiver_socket.async_read(scheduler, bnio::buffer(bytes));
  auto write_sender =
      sender_socket.async_write(scheduler, bnio::buffer(payload), MSG_NOSIGNAL);
  auto read_operation =
      bexec::connect(std::move(read_sender), std::move(read_receiver));
  auto write_operation =
      bexec::connect(std::move(write_sender), std::move(write_receiver));

  bexec::start(read_operation);
  bexec::start(write_operation);
  context.run();

  EXPECT_EQ(completions, 2);
  EXPECT_EQ(read_state->signal, signal_kind::value);
  EXPECT_EQ(read_state->size, payload.size());
  EXPECT_EQ(write_state->signal, signal_kind::value);
  EXPECT_EQ(write_state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(IoContextReadWriteTest, descriptor_pipe_read_write_pair) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int descriptors[2] = {-1, -1};
  EXPECT_EQ(::pipe(descriptors), 0);
  constexpr std::string_view payload = "descriptor pipe";
  std::array<char, 32> bytes{};
  unsigned completions = 0;

  pair_byte_receiver read_receiver;
  read_receiver.context = &context;
  read_receiver.completions = &completions;
  read_receiver.target = 2;
  auto read_state = read_receiver.state;

  pair_byte_receiver write_receiver;
  write_receiver.context = &context;
  write_receiver.completions = &completions;
  write_receiver.target = 2;
  auto write_state = write_receiver.state;

  auto read_sender = scheduler.async_read(
      bnio::async_io::descriptor_view(descriptors[0]), bnio::buffer(bytes));
  auto write_sender = scheduler.async_write(
      bnio::async_io::descriptor_view(descriptors[1]), bnio::buffer(payload));
  auto read_operation =
      bexec::connect(std::move(read_sender), std::move(read_receiver));
  auto write_operation =
      bexec::connect(std::move(write_sender), std::move(write_receiver));

  bexec::start(read_operation);
  bexec::start(write_operation);
  context.run();

  EXPECT_EQ(completions, 2);
  EXPECT_EQ(read_state->signal, signal_kind::value);
  EXPECT_EQ(read_state->size, payload.size());
  EXPECT_EQ(write_state->signal, signal_kind::value);
  EXPECT_EQ(write_state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(IoContextReadWriteTest,
     invalid_descriptor_read_reports_bad_file_descriptor) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  std::array<char, 8> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = scheduler.async_read(bnio::async_io::descriptor_view(),
                                     bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::error_code(EBADF, std::generic_category()));
}

TEST(IoContextReadWriteTest,
     invalid_descriptor_write_reports_bad_file_descriptor) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  constexpr std::string_view payload = "invalid descriptor";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = scheduler.async_write(bnio::async_io::descriptor_view(),
                                      bnio::buffer(payload));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::error_code(EBADF, std::generic_category()));
}

TEST(IoContextReadWriteTest, pre_stopped_descriptor_read_reports_canceled) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  std::array<char, 8> bytes{};
  stopped_byte_receiver receiver;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};
  auto state = receiver.state;

  auto sender = scheduler.async_read(bnio::async_io::descriptor_view(),
                                     bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::make_error_code(std::errc::operation_canceled));
}

TEST(IoContextReadWriteTest, pre_stopped_descriptor_write_reports_canceled) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  constexpr std::string_view payload = "cancelled write";
  stopped_byte_receiver receiver;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};
  auto state = receiver.state;

  auto sender = scheduler.async_write(bnio::async_io::descriptor_view(),
                                      bnio::buffer(payload));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::make_error_code(std::errc::operation_canceled));
}

TEST(IoContextReadWriteTest, file_write_and_read) {
  std::string path = "/tmp/bnio-io-context-file-XXXXXX";
  const int fd = ::mkstemp(path.data());
  EXPECT_TRUE(fd >= 0);
  EXPECT_EQ(::unlink(path.c_str()), 0);

  constexpr std::string_view payload = "file through io_uring";

  {
    bnio::io_context context;
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender =
        bnio::async_write(scheduler, bnio::async_io::descriptor_view(fd),
                          bnio::buffer(payload), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);

    EXPECT_EQ(state->signal, signal_kind::none);
#if defined(BNIO_SYSTEM_BSD)
    std::array<char, 64> started_bytes{};
    EXPECT_EQ(::pread(fd, started_bytes.data(), started_bytes.size(), 0),
              static_cast<ssize_t>(payload.size()));
    EXPECT_TRUE(
        std::memcmp(started_bytes.data(), payload.data(), payload.size()) == 0);
#endif
    context.run();

    EXPECT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, payload.size());
  }

  {
    bnio::io_context context;
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();
    std::array<char, 64> bytes{};

    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = bnio::async_read(
        scheduler, bnio::async_io::descriptor_view(fd), bnio::buffer(bytes), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    EXPECT_EQ(state->signal, signal_kind::none);
#if defined(BNIO_SYSTEM_BSD)
    EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
#endif
    context.run();

    EXPECT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, payload.size());
    EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
  }

  EXPECT_EQ(::close(fd), 0);
}

}  // namespace

// Regression: verify that write-all (repeat_until) works correctly
// for payloads larger than the socket buffer on kqueue.  async_write
// is write-all; async_read_some is single-shot.  Paired in a loop so
// each run() iteration transfers one buffer-ful without the reader
// leaving stale data in the buffer.
TEST(IoContextReadWriteTest, socketpair_70kb_write_all) {
  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket writer_socket(sockets[0]);
  bnio::tcp_socket reader_socket(sockets[1]);

  constexpr std::size_t payload_size = 70 * 1024;
  std::vector<unsigned char> payload(payload_size);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<unsigned char>(i & 0xff);
  }
  std::vector<unsigned char> received(payload.size());

  std::size_t sent = 0;
  std::size_t received_size = 0;
  while (sent < payload_size || received_size < payload_size) {
    bnio::io_context context;
    if (!context_available(context)) {
      return;
    }
    auto scheduler = context.get_post_scheduler();
    unsigned completions = 0;
    const unsigned target = (sent < payload_size) ? 2U : 1U;

    pair_byte_receiver write_recv;
    write_recv.context = &context;
    write_recv.completions = &completions;
    write_recv.target = target;
    auto write_state = write_recv.state;

    pair_byte_receiver read_recv;
    read_recv.context = &context;
    read_recv.completions = &completions;
    read_recv.target = target;
    auto read_state = read_recv.state;

    // Read first so EVFILT_READ is registered before the write fills
    // the buffer, ensuring kevent fires for the available data.
    auto read_op = bexec::connect(
        reader_socket.async_read_some(
            scheduler, bnio::buffer(received.data() + received_size,
                                    received.size() - received_size)),
        std::move(read_recv));
    bexec::start(read_op);

    auto write_op = bexec::connect(
        writer_socket.async_write_some(
            scheduler,
            bnio::buffer(payload.data() + sent, payload.size() - sent),
            MSG_NOSIGNAL),
        std::move(write_recv));
    if (sent < payload.size()) {
      bexec::start(write_op);
    }

    context.run();
    EXPECT_GT(read_state->size, 0);
    received_size += read_state->size;
    if (sent < payload.size()) {
      EXPECT_GT(write_state->size, 0);
      sent += write_state->size;
    }
    if (read_state->size == 0 ||
        (sent < payload.size() && write_state->size == 0)) {
      break;
    }
  }

  EXPECT_EQ(sent, payload_size);
  EXPECT_EQ(received_size, payload_size);
  EXPECT_TRUE(std::memcmp(received.data(), payload.data(), payload.size()) ==
              0);
}

// Positive coverage for set_stopped: io_context::stop() interrupting an
// inflight blocking socket read must produce set_stopped on the receiver.
// All other tests in this file complete reads via data arrival or cancel
// paths (ec=operation_canceled via set_value); this test exercises the pure
// io_context::stop() interruption path.
TEST(IoContextReadWriteTest, inflight_socket_read_aborted_by_io_context_stop) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  // Peer never writes: read blocks forever until interrupted.
  std::array<char, 16> bytes{};

  pair_byte_receiver receiver;
  // context=nullptr, completions=nullptr: receiver does NOT self-stop;
  // the main thread calls context.stop() to interrupt inflight read.
  receiver.context = nullptr;
  receiver.completions = nullptr;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  // Synchronization: post a schedule task that runs once the worker enters
  // the run loop, confirming the worker is active and parked on the read
  // before we call stop().
  std::atomic<bool> worker_active{false};
  struct active_receiver {
    std::atomic<bool>* flag;
    void set_value(std::error_code) noexcept {
      flag->store(true, std::memory_order_release);
    }
    void set_stopped() noexcept {
      flag->store(true, std::memory_order_release);
    }
  };
  auto schedule_sender = context.get_post_scheduler().schedule();
  auto schedule_op =
      bexec::connect(schedule_sender, active_receiver{&worker_active});
  bexec::start(schedule_op);

  std::thread worker([&context] { context.run(); });

  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker_active.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        (void)context.stop();
        worker.join();
        FAIL() << "Timed out waiting for worker to become active";
      }
      std::this_thread::yield();
    }
  }

  // Worker is parked on the blocking read. Interrupt via io_context::stop().
  EXPECT_GE(context.stop(), 0);
  worker.join();

  EXPECT_EQ(state->signal, signal_kind::stopped);
}

// Verifies the set_stopped contract propagates through the write_all
// composite operation: io_context::stop() aborts an inflight write-all loop
// and the outer receiver observes set_stopped (not set_value(ec)).
// The peer never reads, so after the kernel send buffer fills the write_some
// child parks on EVFILT_WRITE; stop() drains inflight I/O via
// complete_submit_stopped, repeat_receiver::set_stopped fires, and the
// write_all_operation forwards set_stopped to its downstream receiver.
TEST(IoContextReadWriteTest, inflight_write_all_aborted_by_io_context_stop) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket sender_socket(sockets[0]);
  bnio::tcp_socket receiver_socket(sockets[1]);  // peer never reads

  // Payload larger than the default Unix socket buffer so write_some parks.
  constexpr std::size_t payload_size = 1 * 1024 * 1024;
  std::vector<char> payload(payload_size, 'x');

  pair_byte_receiver receiver;
  receiver.context = nullptr;
  receiver.completions = nullptr;
  auto state = receiver.state;

  auto sender = sender_socket.async_write(scheduler, bnio::buffer(payload),
                                          MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  std::atomic<bool> worker_active{false};
  struct active_receiver {
    std::atomic<bool>* flag;
    void set_value(std::error_code) noexcept {
      flag->store(true, std::memory_order_release);
    }
    void set_stopped() noexcept {
      flag->store(true, std::memory_order_release);
    }
  };
  auto schedule_sender = context.get_post_scheduler().schedule();
  auto schedule_op =
      bexec::connect(schedule_sender, active_receiver{&worker_active});
  bexec::start(schedule_op);

  std::thread worker([&context] { context.run(); });

  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker_active.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        (void)context.stop();
        worker.join();
        FAIL() << "Timed out waiting for worker to become active";
      }
      std::this_thread::yield();
    }
    // Margin: let the write_some child fill the send buffer and park on
    // EVFILT_WRITE before stop() interrupts.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  EXPECT_GE(context.stop(), 0);
  worker.join();

  EXPECT_EQ(state->signal, signal_kind::stopped);
}
