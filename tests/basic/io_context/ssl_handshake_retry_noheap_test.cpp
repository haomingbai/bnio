/**
 * @file ssl_handshake_retry_noheap_test.cpp
 * @brief No-allocation test for the factory-driven handshake retry operation.
 *
 * The retry operation stores its state and the factory's sender inline. This
 * test counts every global allocation performed while the public sender is
 * connected and started for an invalid stream (a path that reaches the
 * completion machinery without touching OpenSSL), and asserts the count is
 * zero. OpenSSL, context, and stream construction happen before the measured
 * window; the factory is stateless.
 *
 * The pause test measures a callback-driven pause: a preloaded ClientHello
 * plus a client_hello callback returning retry. The factory starts the
 * measurement and resumes the handshake with an empty error code, so the
 * internal transport step for the next want_read (including operation_base's
 * flush/read child setup) runs inside the measured window. The synchronous
 * fake transport completes the read inline with EOF, terminalizing the
 * operation; the memory-BIO work itself does not allocate.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <bnio/ssl/detail/handshake_concepts.h>
#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <atomic>
#include <bexec/just.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../support/io_context/ssl_test_support.h"

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::size_t> g_allocation_count{0};
std::size_t g_factory_calls = 0;

void* counted_allocate(std::size_t size, std::size_t alignment) {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (alignment == 0) {
    void* pointer = std::malloc(size == 0 ? 1 : size);
    if (pointer == nullptr) {
      throw std::bad_alloc();
    }
    return pointer;
  }
  const std::size_t usable = size == 0 ? alignment : size;
  const std::size_t rounded =
      ((usable + alignment - 1) / alignment) * alignment;
  void* pointer = std::aligned_alloc(alignment, rounded);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

struct allocation_probe {
  void begin() noexcept {
    g_allocation_count.store(0, std::memory_order_seq_cst);
    g_count_allocations.store(true, std::memory_order_seq_cst);
  }

  [[nodiscard]] std::size_t end() noexcept {
    g_count_allocations.store(false, std::memory_order_seq_cst);
    return g_allocation_count.load(std::memory_order_seq_cst);
  }
};

struct observation {
  unsigned values = 0;
  unsigned stopped = 0;
  std::error_code error;
};

struct observing_receiver {
  observation* out = nullptr;
  bnio::io_context* context = nullptr;

  void set_value(std::error_code ec) noexcept {
    ++out->values;
    out->error = ec;
    finish();
  }

  void set_stopped() noexcept {
    ++out->stopped;
    finish();
  }

 private:
  void finish() noexcept {
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

// Stateless: no data members, so storing it in the sender is allocation-free.
struct stateless_just_factory {
  auto operator()(const std::error_code&) const {
    ++g_factory_calls;
    return bexec::just(std::error_code{});
  }
};

using stream_type = bnio::ssl::tcp::stream<bnio::tcp_socket>;
using scheduler_type = bnio::io_context::post_scheduler;

// ---------------------------------------------------------------------------
// Inline scheduler and synchronous fake transport. The callback pause and the
// factory invocation run inside start(); the inline scheduler delivers the
// terminal post before start() returns, so the allocation window covers the
// factory invocation and the terminal post.
// ---------------------------------------------------------------------------

struct inline_scheduler {
  bool operator==(const inline_scheduler&) const = default;

  [[nodiscard]] auto schedule() const { return bexec::just(std::error_code{}); }
};

struct sync_io_sender {
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  std::error_code error;
  std::size_t bytes = 0;

  template <class Receiver>
  struct operation {
    Receiver receiver;
    std::error_code error;
    std::size_t bytes = 0;

    void start() noexcept {
      bexec::set_value(std::move(receiver), error, bytes);
    }
  };

  template <class Receiver>
  operation<Receiver> connect(Receiver receiver) && {
    return operation<Receiver>{std::move(receiver), error, bytes};
  }
};

struct sync_transport {
  sync_transport& lowest_layer() noexcept { return *this; }
  const sync_transport& lowest_layer() const noexcept { return *this; }

  template <class Scheduler>
  auto async_write_some(Scheduler&, bnio::const_buffer buffer, int) {
    // Accept the whole pending chunk so the resumed attempt's output flush
    // completes inline; the transport read below then reaches EOF.
    return sync_io_sender{std::error_code{}, buffer.size()};
  }

  template <class Scheduler>
  auto async_read_some(Scheduler&, bnio::mutable_buffer) {
    return sync_io_sender{};
  }
};

using fake_stream_type = bnio::ssl::tcp::stream<sync_transport>;
using generator_stream_type =
    bnio::ssl::tcp::stream<bnio::local::stream_socket>;

// Generates a real ClientHello with a standalone client SSL object. The
// transport endpoint is never used: the hello is written into the server's
// read BIO by the pause test below.
[[nodiscard]] std::vector<char> make_client_hello() {
  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  EXPECT_TRUE(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  generator_stream_type generator{bnio::local::stream_socket(-1),
                                  client_context};
  EXPECT_TRUE(generator.valid());
  SSL* const ssl = generator.native_handle().native_handle();
  EXPECT_NE(ssl, nullptr);
  SSL_set_connect_state(ssl);
  ERR_clear_error();
  const int result = SSL_do_handshake(ssl);
  EXPECT_EQ(result, -1);
  EXPECT_EQ(SSL_get_error(ssl, result), SSL_ERROR_WANT_READ);

  const std::size_t hello_size = BIO_ctrl_pending(generator.native_write_bio());
  EXPECT_GT(hello_size, std::size_t{0});
  std::vector<char> hello(hello_size);
  EXPECT_EQ(BIO_read(generator.native_write_bio(), hello.data(),
                     static_cast<int>(hello.size())),
            static_cast<int>(hello.size()));
  return hello;
}

// Starts the allocation measurement when the factory is reached and resumes
// the handshake with an empty error code. The resumed attempt asks for the
// next transport record, so operation_base's internal flush/read step runs
// inside the measured window.
struct heap_probe_resume_factory {
  allocation_probe* probe = nullptr;
  unsigned* calls = nullptr;
  std::error_code* seen_pause = nullptr;

  auto operator()(const std::error_code& ec) const {
    probe->begin();
    *seen_pause = ec;
    ++*calls;
    return bexec::just(std::error_code{});
  }
};

static_assert(std::is_empty_v<stateless_just_factory>);
static_assert(
    bnio::ssl::detail::handshake_retry_factory<stateless_just_factory>);

}  // namespace

// The retry operation relies on global allocation counting being observable
// in this binary; these replacements exist only in this test executable.
void* operator new(std::size_t size) { return counted_allocate(size, 0); }

void* operator new[](std::size_t size) { return counted_allocate(size, 0); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  return counted_allocate(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return counted_allocate(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return counted_allocate(size, 0);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return counted_allocate(size, 0);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return counted_allocate(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  try {
    return counted_allocate(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* pointer) noexcept { std::free(pointer); }

void operator delete[](void* pointer) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

void operator delete(void* pointer, std::align_val_t) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept {
  std::free(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}

void operator delete(void* pointer, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  std::free(pointer);
}

namespace {

TEST(SslHandshakeRetryNoHeapTest, allocation_probe_counts_allocations) {
  allocation_probe probe;
  probe.begin();
  auto* pointer = new int(7);
  delete pointer;
  const std::size_t measured = probe.end();
  EXPECT_EQ(measured, 1u) << "the allocation probe failed to observe a single "
                             "allocation; the no-heap assertion would be void";
}

TEST(SslHandshakeRetryNoHeapTest,
     connect_and_start_invalid_stream_does_not_allocate) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  // Warm the function-local error category before the measured window; the
  // invalid-stream failure path reports make_no_ssl_error().
  (void)bnio::ssl::make_no_ssl_error();

  // A moved-from stream owns no SSL object: the retry operation must report
  // the dedicated no-OpenSSL-error value without invoking the factory.
  stream_type source{bnio::tcp_socket(-1), ssl_context};
  stream_type owner{std::move(source)};
  ASSERT_FALSE(source.valid());
  ASSERT_TRUE(owner.valid());

  observation observed;
  g_factory_calls = 0;

  allocation_probe probe;
  probe.begin();
  auto sender = source.async_handshake(
      scheduler, bnio::ssl::handshake_type::client, stateless_just_factory{});
  auto operation = bexec::connect(std::move(sender),
                                  observing_receiver{&observed, &context});
  bexec::start(operation);
  const std::size_t allocations = probe.end();

  EXPECT_EQ(allocations, 0u)
      << "connecting and starting the retry operation must not allocate";
  EXPECT_EQ(g_factory_calls, 0u)
      << "the factory must not be invoked without a paused attempt";

  // Deliver the posted completion and verify the public contract.
  context.run();
  EXPECT_EQ(observed.values, 1u);
  EXPECT_EQ(observed.stopped, 0u);
  EXPECT_EQ(observed.error, bnio::ssl::make_no_ssl_error());
  EXPECT_FALSE(bnio::ssl::is_handshake_pause(observed.error));
  EXPECT_EQ(g_factory_calls, 0u);
}

TEST(SslHandshakeRetryNoHeapTest,
     callback_pause_factory_and_transport_step_do_not_allocate) {
  // Certificate loading happens before the measured window.
  test_certificate_files files;
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ssl_context.valid());
  ASSERT_FALSE(ssl_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  ASSERT_FALSE(
      ssl_context.use_private_key_file(files.private_key.string().c_str()));
  ASSERT_FALSE(ssl_context.check_private_key());
  // Retry exactly once so the factory runs; the resumed attempt then succeeds
  // past the callback and reaches the internal transport step.
  unsigned callback_calls = 0;
  ssl_context.set_client_hello_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::alert_out&,
         unsigned* calls) -> bnio::ssl::client_hello_result {
        return ++*calls == 1 ? bnio::ssl::client_hello_result::retry
                             : bnio::ssl::client_hello_result::success;
      },
      &callback_calls);

  // Generate the ClientHello and install the context callback before the
  // measured window: both may allocate.
  const std::vector<char> hello = make_client_hello();
  sync_transport transport;
  fake_stream_type stream{std::move(transport), ssl_context};
  ASSERT_TRUE(stream.valid());
  ASSERT_EQ(BIO_write(stream.native_read_bio(), hello.data(),
                      static_cast<int>(hello.size())),
            static_cast<int>(hello.size()));

  unsigned calls = 0;
  std::error_code seen_pause;
  observation observed;
  allocation_probe probe;

  // The preloaded ClientHello drives the first attempt into the client_hello
  // callback, which retries. The factory starts measuring, records the pause,
  // and resumes the handshake. The resumed attempt asks for the next
  // transport record; the fake transport completes that read inline with EOF,
  // so operation_base's flush/read child setup and the terminal post all run
  // inside the measured window.
  auto sender = stream.async_handshake(
      inline_scheduler{}, bnio::ssl::handshake_type::server,
      heap_probe_resume_factory{&probe, &calls, &seen_pause});
  auto operation =
      bexec::connect(std::move(sender), observing_receiver{&observed});
  bexec::start(operation);
  const std::size_t allocations = probe.end();

  EXPECT_EQ(allocations, 0u)
      << "the factory invocation, the internal transport step, and the "
         "terminal post must not allocate";
  EXPECT_EQ(calls, 1u);
  EXPECT_GE(callback_calls, 2u)
      << "the resumed attempt did not pass the client_hello callback";
  EXPECT_TRUE(bnio::ssl::is_handshake_pause(seen_pause));
  EXPECT_EQ(bnio::ssl::handshake_pause_reason_of(seen_pause),
            bnio::ssl::handshake_pause_reason::client_hello_cb);
  EXPECT_EQ(observed.values, 1u);
  EXPECT_EQ(observed.stopped, 0u);
  // The resumed attempt flushed its flight and the transport read then
  // completed with EOF.
  EXPECT_EQ(observed.error, std::make_error_code(std::errc::connection_reset));
}

}  // namespace
