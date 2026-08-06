// CPU-task fairness probe for bnio.
//
// Focus: fairness of CPU task dispatch and IO completion callbacks on the
// io_context workers (per user: uring itself is secondary).
//
// Modes:
//   order_external  <n> <workers>
//       Main thread posts n detached CPU tasks before run(); records the
//       (value, worker_tid) execution order.  With workers=1 this shows the
//       effective ordering of cross-thread posts (expected FIFO).
//
//   order_nested    <n> <depth> <workers>
//       Every executed task posts a follow-up task (depth levels) from
//       inside the worker, i.e. the worker-local fast path.  Records
//       execution order of the whole tree to expose LIFO/interleaving.
//
//   dist_external   <m> <workers>
//       Main thread posts m tasks, then workers run.  Prints per-worker
//       executed-task counts to expose global-queue bulk-steal imbalance.
//
//   dist_contend    <m> <posters> <workers>
//       `posters` threads race-posting m tasks while workers run.  Prints
//       per-worker counts and per-poster accepted counts.
//
//   io_cpu_delay    <workers> <duration_s> <probe_interval_ms> <conns> <msg>
//       Echo-server style IO load (tcp socket ping-pong) + a periodic
//       CPU-task probe; reports probe completion latency percentiles.
//
// Link against libbnio.a, liburing, ssl/crypto, pthread.

#include <bnio/bnio.h>
#include <bexec/bexec.hpp>

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace bnio;

namespace {

// ---------------------------------------------------------------------------
// Coroutine / sender plumbing (mirrors benchmarks/throughput).
// ---------------------------------------------------------------------------

class detached_task {
 public:
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  struct promise_type {
    detached_task get_return_object() noexcept {
      return detached_task(handle_type::from_promise(*this));
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    struct final_awaiter {
      [[nodiscard]] bool await_ready() const noexcept { return false; }
      void await_suspend(handle_type handle) const noexcept {
        handle.destroy();
      }
      void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept { std::terminate(); }
  };

  explicit detached_task(handle_type handle) noexcept : handle_(handle) {}
  detached_task(const detached_task&) = delete;
  detached_task& operator=(const detached_task&) = delete;
  ~detached_task() { destroy(); }

  void start() noexcept {
    handle_type handle = std::exchange(handle_, {});
    if (handle) handle.resume();
  }

 private:
  void destroy() noexcept {
    if (handle_) {
      handle_.destroy();
      handle_ = {};
    }
  }
  handle_type handle_{};
};

void start(detached_task task) noexcept { task.start(); }

enum class await_status { value, error, stopped };

template <class T>
class await_result {
 public:
  static await_result value(T value) {
    await_result result;
    result.status_ = await_status::value;
    result.value_.emplace(std::move(value));
    return result;
  }
  static await_result error(std::error_code) {
    await_result result;
    result.status_ = await_status::error;
    return result;
  }
  static await_result stopped() {
    await_result result;
    result.status_ = await_status::stopped;
    return result;
  }
  [[nodiscard]] bool has_value() const noexcept {
    return status_ == await_status::value;
  }
  explicit operator bool() const noexcept { return has_value(); }
  T& value() noexcept { return *value_; }
  T&& take_value() noexcept { return std::move(*value_); }

 private:
  await_status status_ = await_status::stopped;
  std::optional<T> value_;
};

template <>
class await_result<void> {
 public:
  static await_result value() {
    await_result result;
    result.status_ = await_status::value;
    return result;
  }
  static await_result error(std::error_code) {
    await_result result;
    result.status_ = await_status::error;
    return result;
  }
  static await_result stopped() {
    await_result result;
    result.status_ = await_status::stopped;
    return result;
  }
  [[nodiscard]] bool has_value() const noexcept {
    return status_ == await_status::value;
  }
  explicit operator bool() const noexcept { return has_value(); }

 private:
  await_status status_ = await_status::stopped;
};

template <class List>
struct sender_value_type;

template <class T>
struct sender_value_type<bexec::type_list<std::tuple<std::error_code, T>>> {
  using type = T;
};

template <>
struct sender_value_type<bexec::type_list<std::tuple<std::error_code>>> {
  using type = void;
};

template <class T>
struct sender_value_type<bexec::type_list<std::tuple<T>>> {
  using type = T;
};

template <>
struct sender_value_type<bexec::type_list<std::tuple<>>> {
  using type = void;
};

template <class Sender>
using sender_value_type_t = typename sender_value_type<
    bexec::detail::sender_value_tuple_list_t<Sender, bexec::empty_env>>::type;

template <class Sender>
class sender_awaiter {
 public:
  using sender_type = std::remove_cvref_t<Sender>;
  using value_type = sender_value_type_t<sender_type>;
  using result_type = await_result<value_type>;

  class receiver {
   public:
    explicit receiver(sender_awaiter& awaiter) noexcept : awaiter_(&awaiter) {}
    [[nodiscard]] bexec::empty_env get_env() const noexcept { return {}; }
    void set_value(std::error_code ec) noexcept
      requires std::is_void_v<value_type>
    {
      if (ec) {
        awaiter_->result_ = result_type::error(ec);
      } else {
        awaiter_->result_ = result_type::value();
      }
      awaiter_->resume();
    }
    template <class Value>
      requires(!std::is_void_v<value_type> &&
               std::constructible_from<value_type, Value>)
    void set_value(std::error_code ec, Value&& value) noexcept {
      if (ec) {
        awaiter_->result_ = result_type::error(ec);
      } else {
        awaiter_->result_ = result_type::value(std::forward<Value>(value));
      }
      awaiter_->resume();
    }
    void set_stopped() noexcept {
      awaiter_->result_ = result_type::stopped();
      awaiter_->resume();
    }

   private:
    sender_awaiter* awaiter_;
  };

  using operation_type = decltype(bexec::connect(std::declval<sender_type&&>(),
                                                 std::declval<receiver>()));

  explicit sender_awaiter(Sender&& sender)
      : operation_(bexec::connect(std::forward<Sender>(sender),
                                  receiver(*this))) {}

  sender_awaiter(const sender_awaiter&) = delete;
  sender_awaiter& operator=(const sender_awaiter&) = delete;
  sender_awaiter(sender_awaiter&&) = delete;
  sender_awaiter& operator=(sender_awaiter&&) = delete;

  [[nodiscard]] bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> continuation) noexcept {
    continuation_ = continuation;
    bexec::start(operation_);
  }
  result_type await_resume() noexcept { return std::move(result_); }

 private:
  void resume() noexcept { continuation_.resume(); }
  std::coroutine_handle<> continuation_;
  result_type result_;
  operation_type operation_;
};

template <class Sender>
auto async_result(Sender&& sender) {
  return sender_awaiter<Sender>(std::forward<Sender>(sender));
}

// ---------------------------------------------------------------------------
// Shared probe state.
// ---------------------------------------------------------------------------

std::mutex g_tid_mutex;
std::unordered_map<std::thread::id, uint32_t> g_tid_map;
uint32_t g_tid_counter = 0;

uint32_t tid_of() {
  std::lock_guard<std::mutex> lock(g_tid_mutex);
  auto [it, inserted] = g_tid_map.emplace(std::this_thread::get_id(), 0);
  if (inserted) it->second = g_tid_counter++;
  return it->second;
}

struct record {
  uint32_t value;
  uint32_t tid;
  std::int64_t t0;  // enqueue ts (ns)
  std::int64_t t1;  // execute ts (ns)
};

std::mutex g_rec_mutex;
std::vector<record> g_records;
std::atomic<std::uint32_t> g_pending{0};

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void push_record(uint32_t value, std::int64_t t0) {
  std::lock_guard<std::mutex> lock(g_rec_mutex);
  g_records.push_back({value, tid_of(), t0, now_ns()});
}

// ---------------------------------------------------------------------------
// Mode: order_external / order_nested / dist_external / dist_contend
// ---------------------------------------------------------------------------

void run_workers(io_context& ctx, unsigned workers) {
  std::vector<std::thread> threads;
  for (unsigned i = 1; i < workers; ++i) {
    threads.emplace_back([&ctx] { ctx.run(); });
  }
  ctx.run();
  for (std::thread& t : threads) t.join();
}

detached_task order_task(io_context& ctx, uint32_t value, std::int64_t t0) {
  auto r = co_await async_result(ctx.get_post_scheduler().schedule());
  if (r) {
    push_record(value, t0);
  }
  if (g_pending.fetch_sub(1) == 1) {
    ctx.stop();
  }
}

// Depth-first follow-up: each executed task spawns one child (value = parent*10
// + child_index within current task) until depth reaches 0.
void spawn_child(io_context& ctx, uint32_t value, int depth, std::int64_t t0);

detached_task nested_leaf(io_context& ctx, uint32_t value, int depth,
                          std::int64_t t0) {
  auto r = co_await async_result(ctx.get_post_scheduler().schedule());
  if (r) {
    push_record(value, t0);
    if (depth > 0) {
      for (uint32_t i = 0; i < 2; ++i) {
        spawn_child(ctx, value * 100 + i, depth - 1, now_ns());
      }
    }
  }
  if (g_pending.fetch_sub(1) == 1) {
    ctx.stop();
  }
}

void spawn_child(io_context& ctx, uint32_t value, int depth, std::int64_t t0) {
  g_pending.fetch_add(1);
  start(nested_leaf(ctx, value, depth, t0));
}

int run_order_external(int argc, char** argv) {
  const unsigned n = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 32;
  const unsigned workers =
      argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1;

  io_context_options opts;
  opts.concurrency_hint = workers;
  opts.platform.entries = 256;
  io_context ctx(opts);
  if (!ctx.is_open()) {
    std::cerr << "ctx unavailable\n";
    return 1;
  }

  for (uint32_t i = 0; i < n; ++i) {
    g_pending.fetch_add(1);
    start(order_task(ctx, i, now_ns()));
  }
  run_workers(ctx, workers);

  std::printf("order_external n=%u workers=%u executed=%zu\n", n, workers,
              g_records.size());
  std::printf("idx value tid latency_us\n");
  for (std::size_t i = 0; i < g_records.size(); ++i) {
    std::printf("%zu %u %u %.1f\n", i, g_records[i].value, g_records[i].tid,
                (g_records[i].t1 - g_records[i].t0) / 1000.0);
  }
  return 0;
}

int run_order_nested(int argc, char** argv) {
  const unsigned n = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 8;
  const int depth = argc > 3 ? std::atoi(argv[3]) : 3;
  const unsigned workers =
      argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 1;

  io_context_options opts;
  opts.concurrency_hint = workers;
  opts.platform.entries = 256;
  io_context ctx(opts);
  if (!ctx.is_open()) {
    std::cerr << "ctx unavailable\n";
    return 1;
  }

  for (uint32_t i = 0; i < n; ++i) {
    g_pending.fetch_add(1);
    start(nested_leaf(ctx, i, depth, now_ns()));
  }
  run_workers(ctx, workers);

  std::printf("order_nested n=%u depth=%d workers=%u executed=%zu\n", n, depth,
              workers, g_records.size());
  std::printf("idx value tid latency_us\n");
  for (std::size_t i = 0; i < g_records.size(); ++i) {
    std::printf("%zu %u %u %.1f\n", i, g_records[i].value, g_records[i].tid,
                (g_records[i].t1 - g_records[i].t0) / 1000.0);
  }
  return 0;
}

detached_task plain_task(io_context& ctx, uint32_t value, std::int64_t t0) {
  auto r = co_await async_result(ctx.get_post_scheduler().schedule());
  if (r) {
    push_record(value, t0);
  }
  if (g_pending.fetch_sub(1) == 1) {
    ctx.stop();
  }
}

int run_dist_external(int argc, char** argv) {
  const unsigned m = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 100000;
  const unsigned workers =
      argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 8;

  io_context_options opts;
  opts.concurrency_hint = workers;
  opts.platform.entries = 512;
  io_context ctx(opts);
  if (!ctx.is_open()) {
    std::cerr << "ctx unavailable\n";
    return 1;
  }

  for (uint32_t i = 0; i < m; ++i) {
    g_pending.fetch_add(1);
    start(plain_task(ctx, i, now_ns()));
  }
  run_workers(ctx, workers);

  std::unordered_map<uint32_t, uint32_t> per_tid;
  for (const record& r : g_records) per_tid[r.tid]++;
  std::printf("dist_external m=%u workers=%u executed=%zu\n", m, workers,
              g_records.size());
  std::printf("tid count pct\n");
  for (const auto& [tid, count] : per_tid) {
    std::printf("%u %u %.2f\n", tid, count, 100.0 * count / m);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Mode: io_cpu_delay — echo-server IO load + periodic CPU-task probe.
// ---------------------------------------------------------------------------

constexpr std::uint16_t k_echo_port = 8092;
constexpr std::size_t k_echo_buffer = 4096;

// Per-connection worker-affinity statistics for the echo server.
struct conn_work {
  uint32_t conn_id;
  uint32_t tid;
};
std::mutex g_conn_mutex;
std::vector<conn_work> g_conn_works;
std::atomic<uint32_t> g_conn_seq{0};

void note_conn_work(uint32_t conn_id) {
  std::lock_guard<std::mutex> lock(g_conn_mutex);
  g_conn_works.push_back({conn_id, tid_of()});
}

detached_task echo_session(io_context& ctx, tcp_socket sk) {
  const uint32_t conn_id = g_conn_seq.fetch_add(1);
  auto scheduler = ctx.get_post_scheduler();
  std::array<char, k_echo_buffer> buf{};
  while (true) {
    auto read_result =
        co_await async_result(sk.async_read_some(scheduler, buffer(buf), 0));
    if (!read_result || read_result.value() == 0) break;
    note_conn_work(conn_id);
    std::size_t send_offset = 0;
    const std::size_t send_size = read_result.value();
    while (send_offset < send_size) {
      auto write_result = co_await async_result(sk.async_write(
          scheduler, const_buffer(buf.data() + send_offset,
                                  send_size - send_offset),
          MSG_NOSIGNAL));
      if (!write_result || write_result.value() == 0) {
        (void)sk.close();
        co_return;
      }
      note_conn_work(conn_id);
      send_offset += write_result.value();
    }
  }
  (void)sk.close();
}

detached_task accept_loop(io_context& ctx, tcp_acceptor& acceptor) {
  auto scheduler = ctx.get_post_scheduler();
  while (true) {
    auto accept_result =
        co_await async_result(acceptor.async_accept(scheduler, SOCK_CLOEXEC));
    if (!accept_result) co_return;
    start(echo_session(ctx, accept_result.take_value()));
  }
}

detached_task cpu_probe_loop(io_context& ctx, steady_timer& timer,
                             unsigned interval_ms, std::int64_t duration_ms,
                             std::vector<std::int64_t>* latencies,
                             std::vector<uint32_t>* tids, std::mutex* mu) {
  const std::int64_t begin = now_ns();
  const std::int64_t budget = duration_ms * 1000000;
  while (now_ns() - begin < budget) {
    timer.expires_after(std::chrono::milliseconds(interval_ms));
    auto timer_result = co_await async_result(timer.async_wait());
    if (!timer_result) break;
    const std::int64_t t0 = now_ns();
    auto sched_result = co_await async_result(ctx.get_post_scheduler().schedule());
    if (!sched_result) break;
    const std::int64_t t1 = now_ns();
    {
      std::lock_guard<std::mutex> lock(*mu);
      latencies->push_back(t1 - t0);
      tids->push_back(tid_of());
    }
  }
  ctx.stop();
}

void print_percentiles(const std::vector<std::int64_t>& samples) {
  if (samples.empty()) {
    std::printf("no samples\n");
    return;
  }
  std::vector<std::int64_t> sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  auto pct = [&](double p) {
    std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1));
    return sorted[idx];
  };
  std::printf("samples=%zu p50=%.0fus p90=%.0fus p99=%.0fus max=%.0fus\n",
              sorted.size(), pct(0.50) / 1000.0, pct(0.90) / 1000.0,
              pct(0.99) / 1000.0, sorted.back() / 1000.0);
  std::int64_t sum = 0;
  for (auto v : sorted) sum += v;
  std::printf("  avg=%.0fus\n",
              static_cast<double>(sum) / sorted.size() / 1000.0);
}

int run_io_cpu_delay(int argc, char** argv) {
  const unsigned workers =
      argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 2;
  const unsigned duration_s =
      argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 10;
  const unsigned interval_ms =
      argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 10;
  const auto port = static_cast<std::uint16_t>(
      argc > 5 ? std::atoi(argv[5]) : k_echo_port);

  io_context_options opts;
  opts.concurrency_hint = workers;
  opts.platform.entries = 1024;
  io_context ctx(opts);
  if (!ctx.is_open()) {
    std::cerr << "ctx unavailable\n";
    return 1;
  }

  tcp_acceptor acceptor;
  if (acceptor.open(ip::tcp::v4())) {
    std::cerr << "acceptor open failed\n";
    return 1;
  }
  (void)acceptor.set_reuse_address(true);
  if (acceptor.bind(ip::endpoint::loopback_v4(port))) {
    std::cerr << "acceptor bind failed\n";
    return 1;
  }
  if (acceptor.listen(512)) {
    std::cerr << "acceptor listen failed\n";
    return 1;
  }

  start(accept_loop(ctx, acceptor));

  steady_timer probe_timer(ctx);
  std::vector<std::int64_t> latencies;
  std::vector<uint32_t> probe_tids;
  std::mutex mu;
  start(cpu_probe_loop(ctx, probe_timer, interval_ms,
                       static_cast<std::int64_t>(duration_s) * 1000,
                       &latencies, &probe_tids, &mu));

  std::printf("io_cpu_delay port=%u workers=%u duration=%us interval=%ums\n",
              port, workers, duration_s, interval_ms);
  run_workers(ctx, workers);

  print_percentiles(latencies);
  std::unordered_map<uint32_t, uint32_t> per_tid;
  for (uint32_t t : probe_tids) per_tid[t]++;
  std::printf("probe executor tid distribution:");
  for (const auto& [tid, count] : per_tid) {
    std::printf(" tid%u=%u", tid, count);
  }
  std::printf("\n");

  std::unordered_map<uint32_t, uint32_t> conn_per_tid;
  std::unordered_map<uint32_t, uint32_t> conn_seen;
  for (const auto& w : g_conn_works) {
    conn_per_tid[w.tid]++;
    conn_seen[w.conn_id] = 1;
  }
  std::printf("echo io completions per worker:");
  for (const auto& [tid, count] : conn_per_tid) {
    std::printf(" tid%u=%u(%.1f%%)", tid, count, 100.0 * count / g_conn_works.size());
  }
  std::printf("\n  unique_conns=%zu total_io=%zu workers_with_io=%zu\n",
              conn_seen.size(), g_conn_works.size(), conn_per_tid.size());
  return 0;
}

int run_dist_contend(int argc, char** argv) {
  const unsigned m = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 200000;
  const unsigned posters =
      argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 4;
  const unsigned workers =
      argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 8;

  io_context_options opts;
  opts.concurrency_hint = workers;
  opts.platform.entries = 512;
  io_context ctx(opts);
  if (!ctx.is_open()) {
    std::cerr << "ctx unavailable\n";
    return 1;
  }

  const unsigned per_poster = m / posters;
  std::vector<std::thread> poster_threads;
  for (unsigned p = 0; p < posters; ++p) {
    poster_threads.emplace_back([&ctx, p, per_poster] {
      const uint32_t base = p * per_poster;
      for (uint32_t i = 0; i < per_poster; ++i) {
        g_pending.fetch_add(1);
        start(plain_task(ctx, base + i, now_ns()));
      }
    });
  }

  std::vector<std::thread> worker_threads;
  for (unsigned i = 1; i < workers; ++i) {
    worker_threads.emplace_back([&ctx] { ctx.run(); });
  }
  ctx.run();
  for (std::thread& t : poster_threads) t.join();
  for (std::thread& t : worker_threads) t.join();

  std::unordered_map<uint32_t, uint32_t> per_tid;
  for (const record& r : g_records) per_tid[r.tid]++;
  std::printf("dist_contend m=%u posters=%u workers=%u executed=%zu\n", m,
              posters, workers, g_records.size());
  std::printf("tid count pct\n");
  for (const auto& [tid, count] : per_tid) {
    std::printf("%u %u %.2f\n", tid, count, 100.0 * count / m);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(
        stderr,
        "usage: %s order_external <n> <workers>\n"
        "       %s order_nested <n> <depth> <workers>\n"
        "       %s dist_external <m> <workers>\n"
        "       %s dist_contend <m> <posters> <workers>\n"
        "       %s io_cpu_delay <workers> <duration_s> <interval_ms> [port]\n",
        argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 2;
  }

  const std::string_view mode(argv[1]);
  if (mode == "order_external") return run_order_external(argc, argv);
  if (mode == "order_nested") return run_order_nested(argc, argv);
  if (mode == "dist_external") return run_dist_external(argc, argv);
  if (mode == "dist_contend") return run_dist_contend(argc, argv);
  if (mode == "io_cpu_delay") return run_io_cpu_delay(argc, argv);

  std::fprintf(stderr, "unknown mode: %s\n", argv[1]);
  return 2;
}
