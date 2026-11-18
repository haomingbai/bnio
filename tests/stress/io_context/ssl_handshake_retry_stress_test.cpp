/**
 * @file ssl_handshake_retry_stress_test.cpp
 * @brief Stress tests for the factory-driven SSL handshake retry API.
 *
 * Everything here uses only the public bnio and bexec sender/receiver
 * surfaces: the 2- and 3-argument async_handshake and the public
 * handshake-pause error category. Transport pauses are driven inside the
 * retry operation; the factory is reached only for callback-driven pauses,
 * which these tests produce with client_hello callbacks returning retry. No
 * bnio detail header is included.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <atomic>
#include <bexec/just.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../support/io_context/ssl_test_support.h"

namespace {

using ssl_stream_type = bnio::ssl::tcp::stream<bnio::local::stream_socket>;
using post_scheduler_type =
    decltype(std::declval<bnio::io_context&>().get_post_scheduler());

[[nodiscard]] bool is_callback_pause(const std::error_code& ec) noexcept {
  return bnio::ssl::is_handshake_pause(ec) &&
         bnio::ssl::handshake_pause_reason_of(ec) ==
             bnio::ssl::handshake_pause_reason::client_hello_cb;
}

struct retry_counters {
  std::atomic<int> factory_calls{0};
  std::atomic<int> pause_violations{0};
  std::atomic<int> reason_violations{0};

  // Event-driven handoff for the cancellation storm: the factory signals
  // when the first pause reached it instead of the test polling with sleeps.
  // The empty critical section in the notifier orders the counter store
  // against a waiter that is between its predicate check and its wait, so no
  // wakeup can be lost.
  std::mutex first_pause_mutex;
  std::condition_variable first_pause;
};

// Counting factory that resumes every callback-driven pause synchronously.
struct counting_resume_factory {
  retry_counters* counters = nullptr;

  auto operator()(const std::error_code& ec) const {
    counters->factory_calls.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> guard(counters->first_pause_mutex);
    }
    counters->first_pause.notify_all();
    // Every factory invocation must be a client_hello callback pause: the
    // transport pauses are internal and never reach the factory.
    if (!bnio::ssl::is_handshake_pause(ec)) {
      counters->pause_violations.fetch_add(1, std::memory_order_relaxed);
    } else if (!is_callback_pause(ec)) {
      counters->reason_violations.fetch_add(1, std::memory_order_relaxed);
    }
    return bexec::just(std::error_code{});
  }
};

struct handshake_outcome {
  std::atomic<int> terminals{0};
  std::atomic<int> success{0};
  std::atomic<int> errors{0};
  std::atomic<int> stopped{0};
  std::atomic<int> pause_leaks{0};
  // Written once before the terminal signal; read after all workers join.
  std::error_code last_error;
};

struct outcome_receiver {
  handshake_outcome* outcome = nullptr;
  std::atomic<int>* total = nullptr;
  int target = 0;
  bnio::io_context* context = nullptr;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      outcome->errors.fetch_add(1, std::memory_order_relaxed);
      outcome->last_error = ec;
      if (bnio::ssl::is_handshake_pause(ec)) {
        outcome->pause_leaks.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      outcome->success.fetch_add(1, std::memory_order_relaxed);
    }
    finish();
  }

  void set_stopped() noexcept {
    outcome->stopped.fetch_add(1, std::memory_order_relaxed);
    finish();
  }

 private:
  void finish() noexcept {
    outcome->terminals.fetch_add(1, std::memory_order_relaxed);
    if (total->fetch_add(1, std::memory_order_relaxed) + 1 == target) {
      (void)context->stop();
    }
  }
};

struct stop_outcome_receiver : outcome_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

// ---------------------------------------------------------------------------
// A factory that completes synchronously with an empty error code for a
// bounded number of rounds, then with a terminal error. The retry driver must
// trampoline these synchronous completions instead of recursing once per
// round.
// ---------------------------------------------------------------------------

struct synchronous_retry_state {
  long remaining = 0;
  std::error_code terminal = std::make_error_code(std::errc::permission_denied);
  std::atomic<long> synchronous_rounds{0};
  std::atomic<long> factory_calls{0};
  std::atomic<long> pause_violations{0};
};

struct synchronous_retry_factory {
  std::shared_ptr<synchronous_retry_state> state;

  auto operator()(const std::error_code& ec) const {
    state->factory_calls.fetch_add(1, std::memory_order_relaxed);
    if (!is_callback_pause(ec)) {
      state->pause_violations.fetch_add(1, std::memory_order_relaxed);
    }
    if (state->remaining > 0) {
      --state->remaining;
      state->synchronous_rounds.fetch_add(1, std::memory_order_relaxed);
      return bexec::just(std::error_code{});
    }
    return bexec::just(state->terminal);
  }
};

// ---------------------------------------------------------------------------
// Shared scaffolding for the multi-pair stress scenarios.
// ---------------------------------------------------------------------------

using client_sender_type =
    decltype(std::declval<ssl_stream_type&>().async_handshake(
        std::declval<post_scheduler_type>(),
        std::declval<bnio::ssl::handshake_type>()));
using server_sender_type =
    decltype(std::declval<ssl_stream_type&>().async_handshake(
        std::declval<post_scheduler_type>(),
        std::declval<bnio::ssl::handshake_type>(),
        std::declval<counting_resume_factory>()));
using client_operation_type = decltype(bexec::connect(
    std::declval<client_sender_type>(), std::declval<outcome_receiver>()));
using server_operation_type = decltype(bexec::connect(
    std::declval<server_sender_type>(), std::declval<outcome_receiver>()));
using cancellable_client_operation_type = decltype(bexec::connect(
    std::declval<client_sender_type>(), std::declval<stop_outcome_receiver>()));

// Per-connection client_hello state: each connection retries its first
// callback invocation and succeeds afterwards, so every handshake produces
// exactly one callback-driven pause.
struct client_hello_retry_state {
  std::mutex mutex;
  std::unordered_set<SSL*> retried;
};

// Owns the certificate files and the configured TLS contexts used by the
// stress scenarios.
struct stress_ssl_fixture {
  test_certificate_files files;

  bnio::ssl::context server_context{bnio::ssl::context_method::tls_server};
  bnio::ssl::context client_context{bnio::ssl::context_method::tls_client};
  std::shared_ptr<client_hello_retry_state> hello_retry =
      std::make_shared<client_hello_retry_state>();

  stress_ssl_fixture() {
    test_require(server_context.valid());
    test_require(!server_context.use_certificate_chain_file(
        files.certificate.string().c_str()));
    test_require(!server_context.use_private_key_file(
        files.private_key.string().c_str()));
    test_require(!server_context.check_private_key());

    // Keyed by native_handle(): a stable per-connection key. Each connection
    // pauses exactly once, on its first client_hello callback invocation.
    server_context.set_client_hello_callback(
        [state = hello_retry](
            bnio::ssl::context_view, bnio::ssl::stream_view ssl,
            bnio::ssl::alert_out&) -> bnio::ssl::client_hello_result {
          std::lock_guard<std::mutex> guard(state->mutex);
          if (state->retried.insert(ssl.native_handle()).second) {
            return bnio::ssl::client_hello_result::retry;
          }
          return bnio::ssl::client_hello_result::success;
        });

    test_require(client_context.valid());
    client_context.set_verify_mode(SSL_VERIFY_NONE);
  }
};

// Heap-owns one client/server socket pair per scenario pair.
struct stream_pool {
  std::vector<std::unique_ptr<ssl_stream_type>> clients;
  std::vector<std::unique_ptr<ssl_stream_type>> servers;
};

stream_pool make_stream_pool(int pairs, stress_ssl_fixture& fixture) {
  stream_pool pool;
  pool.clients.reserve(static_cast<std::size_t>(pairs));
  pool.servers.reserve(static_cast<std::size_t>(pairs));
  for (int i = 0; i < pairs; ++i) {
    int sockets[2] = {-1, -1};
    test_require(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    pool.clients.push_back(std::make_unique<ssl_stream_type>(
        bnio::local::stream_socket(sockets[0]), fixture.client_context));
    pool.servers.push_back(std::make_unique<ssl_stream_type>(
        bnio::local::stream_socket(sockets[1]), fixture.server_context));
  }
  return pool;
}

std::vector<std::unique_ptr<bexec::inplace_stop_source>> make_stop_sources(
    int count) {
  std::vector<std::unique_ptr<bexec::inplace_stop_source>> sources;
  sources.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    sources.push_back(std::make_unique<bexec::inplace_stop_source>());
  }
  return sources;
}

std::unique_ptr<std::unique_ptr<handshake_outcome>[]> make_outcome_array(
    int count) {
  auto outcomes = std::make_unique<std::unique_ptr<handshake_outcome>[]>(
      static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    outcomes[static_cast<std::size_t>(i)] =
        std::make_unique<handshake_outcome>();
  }
  return outcomes;
}

struct outcome_failures {
  int terminal = 0;
  int kind = 0;
  int success = 0;
  int error = 0;
  int stopped = 0;
  int pause_leaks = 0;
};

outcome_failures count_outcome_failures(
    const std::unique_ptr<std::unique_ptr<handshake_outcome>[]>& outcomes,
    int count) {
  outcome_failures failures;
  for (int i = 0; i < count; ++i) {
    const auto& outcome = *outcomes[static_cast<std::size_t>(i)];
    const int terminals = outcome.terminals.load(std::memory_order_acquire);
    const int success = outcome.success.load(std::memory_order_acquire);
    const int errors = outcome.errors.load(std::memory_order_acquire);
    const int stopped = outcome.stopped.load(std::memory_order_acquire);
    if (terminals != 1) {
      ++failures.terminal;
    }
    if (success + errors + stopped != terminals) {
      ++failures.kind;
    }
    if (success != 1) {
      ++failures.success;
    }
    if (errors != 0) {
      ++failures.error;
    }
    if (stopped != 0) {
      ++failures.stopped;
    }
    failures.pause_leaks += outcome.pause_leaks.load(std::memory_order_acquire);
  }
  return failures;
}

struct cancellation_failures {
  int terminal = 0;
  int double_completions = 0;
  int kind = 0;
  int pause_leaks = 0;
};

cancellation_failures count_cancellation_failures(
    const std::unique_ptr<std::unique_ptr<handshake_outcome>[]>& outcomes,
    int count) {
  cancellation_failures failures;
  for (int i = 0; i < count; ++i) {
    const auto& outcome = *outcomes[static_cast<std::size_t>(i)];
    const int terminals = outcome.terminals.load(std::memory_order_acquire);
    const int kinds = outcome.success.load(std::memory_order_acquire) +
                      outcome.errors.load(std::memory_order_acquire) +
                      outcome.stopped.load(std::memory_order_acquire);
    if (terminals != 1) {
      ++failures.terminal;
    }
    if (terminals > 1) {
      ++failures.double_completions;
    }
    if (kinds != terminals) {
      ++failures.kind;
    }
    failures.pause_leaks += outcome.pause_leaks.load(std::memory_order_acquire);
  }
  return failures;
}

// Builds one client/server pair's operations, preserving the outcome-index
// mapping (client = i*2, server = i*2+1) and heap ownership of every
// operation.
void add_handshake_pair(
    int i, post_scheduler_type scheduler, retry_counters& counters,
    std::atomic<int>& total, int target, bnio::io_context& context,
    stream_pool& pool,
    std::vector<std::unique_ptr<client_operation_type>>& client_operations,
    std::vector<std::unique_ptr<server_operation_type>>& server_operations,
    std::unique_ptr<std::unique_ptr<handshake_outcome>[]>& outcomes) {
  const std::size_t client_index = static_cast<std::size_t>(i * 2);
  const std::size_t server_index = client_index + 1;
  outcome_receiver client_receiver{outcomes[client_index].get(), &total, target,
                                   &context};
  outcome_receiver server_receiver{outcomes[server_index].get(), &total, target,
                                   &context};

  auto client_sender =
      pool.clients[static_cast<std::size_t>(i)]->async_handshake(
          scheduler, bnio::ssl::handshake_type::client);
  client_operations.emplace_back(new client_operation_type(
      bexec::connect(std::move(client_sender), std::move(client_receiver))));

  auto server_sender =
      pool.servers[static_cast<std::size_t>(i)]->async_handshake(
          scheduler, bnio::ssl::handshake_type::server,
          counting_resume_factory{&counters});
  server_operations.emplace_back(new server_operation_type(
      bexec::connect(std::move(server_sender), std::move(server_receiver))));
}

// Builds one cancellable client/server pair. The stop-source token is copied
// into the receiver's environment, so the owned stop sources must outlive the
// operations. The receiver's terminal threshold is unreachable: only the main
// thread stops this test's context.
void add_cancellation_pair(
    int i, post_scheduler_type scheduler, retry_counters& counters,
    std::atomic<int>& total, bnio::io_context& context, stream_pool& pool,
    std::vector<std::unique_ptr<bexec::inplace_stop_source>>& stop_sources,
    std::vector<std::unique_ptr<cancellable_client_operation_type>>&
        client_operations,
    std::vector<std::unique_ptr<server_operation_type>>& server_operations,
    std::unique_ptr<std::unique_ptr<handshake_outcome>[]>& outcomes) {
  const std::size_t client_index = static_cast<std::size_t>(i * 2);
  const std::size_t server_index = client_index + 1;

  // The main thread owns the stop for this test. Reaching a terminal-count
  // threshold inside a worker would call io_context::stop() from the drain
  // that the winning stop is waiting on, which deadlocks; use an unreachable
  // threshold so only the main thread stops the context.
  constexpr int kNoReceiverStop = std::numeric_limits<int>::max();

  stop_outcome_receiver client_receiver;
  client_receiver.outcome = outcomes[client_index].get();
  client_receiver.total = &total;
  client_receiver.target = kNoReceiverStop;
  client_receiver.context = &context;
  client_receiver.env =
      stop_env{stop_sources[static_cast<std::size_t>(i)]->get_token()};

  outcome_receiver server_receiver{outcomes[server_index].get(), &total,
                                   kNoReceiverStop, &context};

  auto client_sender =
      pool.clients[static_cast<std::size_t>(i)]->async_handshake(
          scheduler, bnio::ssl::handshake_type::client);
  client_operations.emplace_back(new cancellable_client_operation_type(
      bexec::connect(std::move(client_sender), std::move(client_receiver))));

  auto server_sender =
      pool.servers[static_cast<std::size_t>(i)]->async_handshake(
          scheduler, bnio::ssl::handshake_type::server,
          counting_resume_factory{&counters});
  server_operations.emplace_back(new server_operation_type(
      bexec::connect(std::move(server_sender), std::move(server_receiver))));
}

// Starts every operation and runs the workers until all handshakes complete,
// with the bounded watchdog of the original scenario. All threads are joined
// before returning.
void run_handshake_storm(
    bnio::io_context& context, unsigned worker_count,
    std::vector<std::unique_ptr<client_operation_type>>& client_operations,
    std::vector<std::unique_ptr<server_operation_type>>& server_operations) {
  for (auto& operation : client_operations) {
    bexec::start(*operation);
  }
  for (auto& operation : server_operations) {
    bexec::start(*operation);
  }

  std::atomic<bool> finished{false};
  std::thread watchdog([&context, &finished] {
    for (int i = 0; i < 400; ++i) {
      if (finished.load(std::memory_order_acquire)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    (void)context.stop();
  });

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    workers.emplace_back([&context] { (void)context.run(); });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  finished.store(true, std::memory_order_release);
  watchdog.join();
}

// Starts every operation, spawns the workers, waits (bounded) for the first
// factory call, requests stop on every source, stops the context, and joins
// every worker before returning.
void run_cancellation_storm(
    bnio::io_context& context, unsigned worker_count, retry_counters& counters,
    std::vector<std::unique_ptr<cancellable_client_operation_type>>&
        client_operations,
    std::vector<std::unique_ptr<server_operation_type>>& server_operations,
    std::vector<std::unique_ptr<bexec::inplace_stop_source>>& stop_sources) {
  for (auto& operation : client_operations) {
    bexec::start(*operation);
  }
  for (auto& operation : server_operations) {
    bexec::start(*operation);
  }

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    workers.emplace_back([&context] { (void)context.run(); });
  }

  // Wait for the first retry factory invocation (event-driven: the factory
  // notifies; no polling sleeps), then hit every stop token and stop the
  // context while the transports are still parked. The bounded wait only
  // guards against a stranded operation; the assertion below reports it.
  {
    std::unique_lock<std::mutex> lock(counters.first_pause_mutex);
    (void)counters.first_pause.wait_for(
        lock, std::chrono::seconds(5), [&counters] {
          return counters.factory_calls.load(std::memory_order_acquire) != 0;
        });
  }
  for (auto& source : stop_sources) {
    (void)source->request_stop();
  }
  (void)context.stop();

  for (auto& worker : workers) {
    worker.join();
  }
}

// ---------------------------------------------------------------------------

TEST(SslHandshakeRetryStressTest,
     many_concurrent_handshakes_with_counting_factory) {
  constexpr int kPairs = 16;
  constexpr unsigned kWorkers = 4;
  constexpr int kTarget = kPairs * 2;

  stress_ssl_fixture fixture;

  bnio::io_context_options options;
  options.concurrency_hint = kWorkers;
  bnio::io_context context(options);
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  stream_pool pool = make_stream_pool(kPairs, fixture);
  auto outcomes = make_outcome_array(kTarget);

  retry_counters counters;
  std::atomic<int> total{0};

  std::vector<std::unique_ptr<client_operation_type>> client_operations;
  std::vector<std::unique_ptr<server_operation_type>> server_operations;
  client_operations.reserve(static_cast<std::size_t>(kPairs));
  server_operations.reserve(static_cast<std::size_t>(kPairs));

  for (int i = 0; i < kPairs; ++i) {
    add_handshake_pair(i, scheduler, counters, total, kTarget, context, pool,
                       client_operations, server_operations, outcomes);
  }

  run_handshake_storm(context, kWorkers, client_operations, server_operations);

  const outcome_failures failures = count_outcome_failures(outcomes, kTarget);

  EXPECT_EQ(total.load(std::memory_order_acquire), kTarget);
  EXPECT_EQ(failures.terminal, 0) << "handshakes without exactly one signal";
  EXPECT_EQ(failures.kind, 0)
      << "handshakes whose completion kinds disagree with their terminal count";
  EXPECT_EQ(failures.success, 0) << "handshakes that did not succeed";
  EXPECT_EQ(failures.error, 0) << "handshakes that reported an error";
  EXPECT_EQ(failures.stopped, 0) << "handshakes that reported stopped";
  EXPECT_EQ(failures.pause_leaks, 0)
      << "pause codes leaked to a public receiver";
  // Each connection's client_hello callback retries exactly once, so the
  // factory sees exactly one callback-driven pause per connection. Transport
  // pauses no longer contribute invocations.
  EXPECT_EQ(counters.factory_calls.load(std::memory_order_acquire), kPairs);
  EXPECT_EQ(counters.pause_violations.load(std::memory_order_acquire), 0);
  EXPECT_EQ(counters.reason_violations.load(std::memory_order_acquire), 0);
}

TEST(SslHandshakeRetryStressTest,
     synchronous_retry_trampoline_does_not_overflow_stack) {
  constexpr long kSynchronousRounds = 100000;

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());
  // Always retry: every server attempt pauses in the client_hello callback,
  // so the factory is invoked once per synchronous round.
  server_context.set_client_hello_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::alert_out&) -> bnio::ssl::client_hello_result {
        return bnio::ssl::client_hello_result::retry;
      });

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);
  ssl_stream_type client{bnio::local::stream_socket(sockets[0]),
                         client_context};
  ssl_stream_type server{bnio::local::stream_socket(sockets[1]),
                         server_context};

  // The factory completes synchronously with an empty sender for
  // kSynchronousRounds rounds, then with a terminal error. All rounds run on
  // one driver stack: the retry operation must trampoline them instead of
  // recursing once per round.
  auto state = std::make_shared<synchronous_retry_state>();
  state->remaining = kSynchronousRounds;

  constexpr int kNoReceiverStop = std::numeric_limits<int>::max();
  std::atomic<int> total{0};
  auto client_outcome = std::make_unique<handshake_outcome>();
  auto server_outcome = std::make_unique<handshake_outcome>();
  // The server terminal (the factory's terminal error) stops the context; the
  // client's parked internal transport read is then aborted, so both
  // operations terminate exactly once.
  outcome_receiver client_receiver{client_outcome.get(), &total,
                                   kNoReceiverStop, &context};
  outcome_receiver server_receiver{server_outcome.get(), &total, 1, &context};

  auto client_sender =
      client.async_handshake(scheduler, bnio::ssl::handshake_type::client);
  auto server_sender =
      server.async_handshake(scheduler, bnio::ssl::handshake_type::server,
                             synchronous_retry_factory{state});

  auto client_operation =
      bexec::connect(std::move(client_sender), std::move(client_receiver));
  auto server_operation =
      bexec::connect(std::move(server_sender), std::move(server_receiver));

  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();

  EXPECT_EQ(state->synchronous_rounds.load(std::memory_order_acquire),
            kSynchronousRounds);
  // One factory invocation per synchronous round, plus the terminal round.
  EXPECT_EQ(state->factory_calls.load(std::memory_order_acquire),
            kSynchronousRounds + 1);
  EXPECT_EQ(state->pause_violations.load(std::memory_order_acquire), 0);

  EXPECT_EQ(server_outcome->terminals.load(std::memory_order_acquire), 1);
  EXPECT_EQ(server_outcome->errors.load(std::memory_order_acquire), 1);
  EXPECT_EQ(server_outcome->success.load(std::memory_order_acquire), 0);
  EXPECT_EQ(server_outcome->stopped.load(std::memory_order_acquire), 0);
  EXPECT_EQ(server_outcome->last_error, state->terminal);

  EXPECT_EQ(client_outcome->terminals.load(std::memory_order_acquire), 1);
  EXPECT_EQ(total.load(std::memory_order_acquire), 2);
}

TEST(SslHandshakeRetryStressTest, cancellation_storm_completes_exactly_once) {
  constexpr int kPairs = 16;
  constexpr unsigned kWorkers = 4;
  constexpr int kTarget = kPairs * 2;

  stress_ssl_fixture fixture;

  bnio::io_context_options options;
  options.concurrency_hint = kWorkers;
  bnio::io_context context(options);
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  stream_pool pool = make_stream_pool(kPairs, fixture);
  auto stop_sources = make_stop_sources(kPairs);
  auto outcomes = make_outcome_array(kTarget);

  retry_counters counters;
  std::atomic<int> total{0};

  std::vector<std::unique_ptr<cancellable_client_operation_type>>
      client_operations;
  std::vector<std::unique_ptr<server_operation_type>> server_operations;
  client_operations.reserve(static_cast<std::size_t>(kPairs));
  server_operations.reserve(static_cast<std::size_t>(kPairs));

  for (int i = 0; i < kPairs; ++i) {
    add_cancellation_pair(i, scheduler, counters, total, context, pool,
                          stop_sources, client_operations, server_operations,
                          outcomes);
  }

  run_cancellation_storm(context, kWorkers, counters, client_operations,
                         server_operations, stop_sources);

  const cancellation_failures failures =
      count_cancellation_failures(outcomes, kTarget);

  EXPECT_EQ(total.load(std::memory_order_acquire), kTarget);
  EXPECT_EQ(failures.terminal, 0)
      << "operations without exactly one terminal signal after cancellation";
  EXPECT_EQ(failures.double_completions, 0)
      << "operations that completed more than once after cancellation";
  EXPECT_EQ(failures.kind, 0)
      << "operations whose completion kinds disagree with their terminal "
         "count after cancellation";
  EXPECT_EQ(failures.pause_leaks, 0)
      << "pause codes leaked to a public receiver";
  EXPECT_GE(counters.factory_calls.load(std::memory_order_acquire), 1);
  EXPECT_EQ(counters.pause_violations.load(std::memory_order_acquire), 0);
  EXPECT_EQ(counters.reason_violations.load(std::memory_order_acquire), 0);
}

}  // namespace
