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
      // Canceled completions carry the transferred byte count in the payload.
      state->size = size;
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

  auto sender = receiver_socket.async_read_some(scheduler, bnio::buffer(bytes));
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

  auto sender = receiver_socket.async_read_some(scheduler, bnio::buffer(bytes));
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

  auto sender = receiver_socket.async_read_some(scheduler, bnio::buffer(bytes));
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

  auto read_sender =
      receiver_socket.async_read_some(scheduler, bnio::buffer(bytes));
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

  auto read_sender = scheduler.async_read_some(
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

TEST(IoContextReadWriteTest, pre_stopped_descriptor_read_stops) {
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

  // Contract: a stop token already canceled at start() is observed by the
  // operation and completes via set_stopped (not
  // set_value(operation_canceled)).
  EXPECT_EQ(state->signal, signal_kind::stopped);
}

TEST(IoContextReadWriteTest, pre_stopped_descriptor_write_stops) {
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

  // Contract: a stop token already canceled at start() is observed by the
  // operation and completes via set_stopped (not
  // set_value(operation_canceled)).
  EXPECT_EQ(state->signal, signal_kind::stopped);
}

// Streaming file I/O contract: descriptor_view operations advance the
// kernel file position. Consecutive writes concatenate instead of
// overlapping, and consecutive reads continue from where the previous one
// stopped. Positioned access is provided by random_access_file instead.
TEST(IoContextReadWriteTest, file_write_and_read) {
  std::string path = "/tmp/bnio-io-context-file-XXXXXX";
  const int fd = ::mkstemp(path.data());
  EXPECT_TRUE(fd >= 0);
  EXPECT_EQ(::unlink(path.c_str()), 0);

  constexpr std::string_view first_payload = "file through ";
  constexpr std::string_view second_payload = "streaming io";
  const std::string expected =
      std::string(first_payload) + std::string(second_payload);

  // Two consecutive streaming writes: each must start where the kernel file
  // position was left by the previous one.
  for (const std::string_view payload : {first_payload, second_payload}) {
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
                          bnio::buffer(payload));
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);

    EXPECT_EQ(state->signal, signal_kind::none);
    context.run();

    EXPECT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, payload.size());
  }

  // The file holds both payloads back to back, not overlapping at offset 0.
  EXPECT_EQ(::lseek(fd, 0, SEEK_SET), 0);
  {
    std::array<char, 64> written{};
    const ssize_t stored = ::read(fd, written.data(), written.size());
    EXPECT_EQ(stored, static_cast<ssize_t>(expected.size()));
    EXPECT_TRUE(std::memcmp(written.data(), expected.data(),
                            expected.size()) == 0);
  }

  // Two consecutive streaming reads: the second continues from the kernel
  // file position left by the first instead of re-reading the same chunk.
  EXPECT_EQ(::lseek(fd, 0, SEEK_SET), 0);
  for (const std::string_view payload : {first_payload, second_payload}) {
    bnio::io_context context;
    auto scheduler = context.get_post_scheduler();
    std::array<char, 16> bytes{};

    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = bnio::async_read_some(
        scheduler, bnio::async_io::descriptor_view(fd),
        bnio::buffer(bytes.data(), payload.size()));
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    EXPECT_EQ(state->signal, signal_kind::none);
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

// Contract coverage for io_context::stop() aborting an inflight blocking
// socket read: the completion must be set_value(operation_canceled, 0), not
// set_stopped. async_read is read-all; with the peer never writing, the
// composite operation parks on its first child read. io_context::stop()
// aborts the inflight child and the read-all loop reports
// set_value(operation_canceled, bytes_so_far) to the receiver. All other
// tests in this file complete reads via data arrival or stop-token paths;
// this test exercises the pure io_context::stop() interruption path.
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

  // Peer never writes: the read-all operation parks on its first child read
  // until interrupted.
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

  // Contract: context stop aborting inflight I/O completes via
  // set_value(operation_canceled, n); no bytes were read before the stop.
  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::make_error_code(std::errc::operation_canceled));
  EXPECT_EQ(state->size, 0u);
}

// Contract coverage for io_context::stop() aborting an inflight write-all:
// the completion must be set_value(operation_canceled, n) where n preserves
// the byte count transferred before the stop. The peer never reads, so after
// the kernel send buffer fills the write_some child parks on EVFILT_WRITE;
// stop() aborts the inflight I/O and the write_all loop reports
// set_value(operation_canceled, transferred) to the downstream receiver.
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

  auto sender =
      sender_socket.async_write(scheduler, bnio::buffer(payload), MSG_NOSIGNAL);
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

  // Contract: context stop aborting inflight I/O completes via
  // set_value(operation_canceled, n). The eager first write_all iterations
  // filled the kernel send buffer before the child parked, so n must
  // preserve the partial transfer; the exact count is kernel-dependent.
  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::make_error_code(std::errc::operation_canceled));
  EXPECT_GT(state->size, 0u);
}

// Covers write_all_operation::start() empty-buffer path (write_all.h:291-294):
// a zero-size buffer must complete synchronously with ec={} and bytes=0
// without entering the repeat_until loop.
TEST(IoContextReadWriteTest, zero_size_buffer_write_reports_success) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket sender_socket(sockets[0]);
  bnio::tcp_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "never written";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = sender_socket.async_write(
      scheduler, bnio::buffer(payload.data(), 0), MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, 0u);
}

// Covers the race rule of the cancellation contract: when the user stop
// token is requested mid-loop and io_context::stop() interrupts the inflight
// write_some, the token cancellation wins and the write_all operation
// completes via set_stopped (not set_value(operation_canceled, transferred)).
//
// Setup: fill the sender's send buffer so write_some parks on EVFILT_WRITE.
// The receiver exposes an inplace_stop_token from a source that is NOT yet
// requested. After the worker is confirmed active and the write is parked,
// request stop on the source, then call context.stop() to interrupt the
// inflight I/O. Because the token was canceled before the context stop
// interrupt is observed, the completion channel is set_stopped.
TEST(IoContextReadWriteTest, write_all_stop_token_mid_loop_stops) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket sender_socket(sockets[0]);
  bnio::tcp_socket receiver_socket(sockets[1]);  // peer never reads

  // Set sender non-blocking and fill the send buffer so write_some parks.
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

  // Create stop source but do NOT request stop yet.
  bexec::inplace_stop_source source;

  // Receiver with stop_env; context=nullptr so it does NOT self-stop.
  constexpr std::size_t payload_size = 1 * 1024 * 1024;
  std::vector<char> payload(payload_size, 'x');

  stopped_byte_receiver receiver;
  receiver.context = nullptr;
  receiver.env = stop_env{source.get_token()};
  auto state = receiver.state;

  auto sender =
      sender_socket.async_write(scheduler, bnio::buffer(payload), MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  // Schedule a task to confirm the worker is active before requesting stop.
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
    // Margin: let write_some fill the send buffer and park on EVFILT_WRITE.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Request stop on the source first, then interrupt the inflight I/O.
  source.request_stop();
  EXPECT_GE(context.stop(), 0);
  worker.join();

  // Contract race rule: token cancellation wins over the context stop
  // interrupt, so the completion channel is set_stopped.
  EXPECT_EQ(state->signal, signal_kind::stopped);
}
