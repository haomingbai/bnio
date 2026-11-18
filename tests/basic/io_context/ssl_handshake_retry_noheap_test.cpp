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
 */

#include <bnio/ssl.h>
#include <bnio/ssl/detail/handshake_concepts.h>
#include <gtest/gtest.h>

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
// Inline scheduler and synchronous fake transport: the pause flush, factory
// invocation, wait step, and terminal post all run inside start(), which lets
// the allocation window cover the whole pause -> factory -> wait cycle.
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
  bool accept_all_writes = false;

  sync_transport& lowest_layer() noexcept { return *this; }
  const sync_transport& lowest_layer() const noexcept { return *this; }

  template <class Scheduler>
  auto async_write_some(Scheduler&, bnio::const_buffer buffer, int) {
    const std::size_t accepted = accept_all_writes ? buffer.size() : 0;
    return sync_io_sender{std::error_code{}, accepted};
  }

  template <class Scheduler>
  auto async_read_some(Scheduler&, bnio::mutable_buffer) {
    // Zero bytes with an empty error is transport EOF: the wait step reports
    // connection_reset, which terminalizes the retry operation.
    return sync_io_sender{};
  }
};

using fake_stream_type = bnio::ssl::tcp::stream<sync_transport>;

struct heap_probe_wait_factory {
  inline_scheduler scheduler;
  fake_stream_type* stream = nullptr;
  allocation_probe* probe = nullptr;
  unsigned* calls = nullptr;

  auto operator()(const std::error_code& ec) const {
    // Start measuring exactly when the factory is reached: the first SSL
    // attempt (and its OpenSSL allocations) already happened.
    probe->begin();
    ++*calls;
    return bnio::ssl::make_handshake_wait_sender(scheduler, *stream, ec);
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
     pause_factory_wait_step_and_terminal_post_do_not_allocate) {
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  sync_transport transport;
  transport.accept_all_writes = true;
  fake_stream_type stream{std::move(transport), ssl_context};
  ASSERT_TRUE(stream.valid());

  unsigned calls = 0;
  observation observed;
  allocation_probe probe;

  // The inline scheduler delivers the terminal post inside start(); the fake
  // transport completes the pause flush and the wait read synchronously.
  auto sender = stream.async_handshake(
      inline_scheduler{}, bnio::ssl::handshake_type::client,
      heap_probe_wait_factory{inline_scheduler{}, &stream, &probe, &calls});
  auto operation =
      bexec::connect(std::move(sender), observing_receiver{&observed});
  bexec::start(operation);
  const std::size_t allocations = probe.end();

  EXPECT_EQ(allocations, 0u)
      << "the pause -> factory -> wait step and terminal post must not "
         "allocate";
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(observed.values, 1u);
  EXPECT_EQ(observed.stopped, 0u);
  EXPECT_EQ(observed.error, std::make_error_code(std::errc::connection_reset));
}

}  // namespace
