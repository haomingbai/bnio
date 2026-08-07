/**
 * CPU fairness benchmark: N randomly-sized counting tasks, pre-generated
 * workload sequence, posted via detached coroutines.
 *
 * Each coroutine suspends on schedule(), then busy-counts to its
 * pre-assigned limit on whichever worker resumes it.  The coroutine
 * frame holds the operation state inline (single allocation per task),
 * and per-worker tracking uses lock-free atomic counters keyed by
 * std::thread::id.
 */
#include <bnio/bnio.h>

#include <algorithm>
#include <atomic>
#include <bexec/bexec.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>
#include <vector>

using namespace bnio;

namespace {

constexpr unsigned k_default_workers = 4;
constexpr unsigned k_default_tasks  = 20000;
constexpr unsigned k_min_count      = 10000;
constexpr unsigned k_max_count      = 1000000;
constexpr unsigned k_max_workers    = 32;

// ---- argument parsing ----

unsigned long parse_ulong(const char* arg, unsigned long fallback) {
  if (arg == nullptr) return fallback;
  char* end = nullptr;
  unsigned long v = std::strtoul(arg, &end, 10);
  if (end == arg || *end != '\0' || v == 0) return fallback;
  return v;
}

// ---- pre-generated workloads ----

std::vector<unsigned> generate_workloads(unsigned count, unsigned seed = 42) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<unsigned> dist(k_min_count, k_max_count);
  std::vector<unsigned> w(count);
  for (auto& v : w) v = dist(rng);
  return w;
}

// ---- per-worker tracking ----

struct worker_stats {
  std::atomic<std::uint64_t> tasks{0};
  std::atomic<std::uint64_t> total_work{0};
};

worker_stats*      g_stats      = nullptr;
std::thread::id*   g_worker_ids = nullptr;
std::mutex         g_reg_mutex;
unsigned           g_max_workers = k_max_workers;
unsigned           g_slot = 0;

void register_worker_thread() {
  std::lock_guard<std::mutex> lk(g_reg_mutex);
  if (g_slot < g_max_workers) g_worker_ids[g_slot++] = std::this_thread::get_id();
}

int find_worker_id() {
  auto tid = std::this_thread::get_id();
  for (unsigned i = 0; i < g_slot; ++i)
    if (g_worker_ids[i] == tid) return static_cast<int>(i);
  return -1;
}

// ---- coroutine plumbing (from bnio_throughput_benchmark) ----

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
      bool await_ready() const noexcept { return false; }
      void await_suspend(handle_type h) const noexcept { h.destroy(); }
      void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept { std::terminate(); }
  };

  explicit detached_task(handle_type h) noexcept : handle_(h) {}
  detached_task(const detached_task&) = delete;
  detached_task& operator=(const detached_task&) = delete;
  ~detached_task() { destroy(); }

  void start() noexcept {
    handle_type h = std::exchange(handle_, {});
    if (h) h.resume();
  }

 private:
  void destroy() noexcept {
    if (handle_) { handle_.destroy(); handle_ = {}; }
  }
  handle_type handle_{};
};

void start(detached_task t) noexcept { t.start(); }

enum class await_status { value, error, stopped };

template <class T>
class await_result {
 public:
  static await_result value(T v) { await_result r; r.status_ = await_status::value; r.value_.emplace(std::move(v)); return r; }
  static await_result error(std::error_code) { await_result r; r.status_ = await_status::error; return r; }
  static await_result stopped() { await_result r; r.status_ = await_status::stopped; return r; }
  bool has_value() const noexcept { return status_ == await_status::value; }
  explicit operator bool() const noexcept { return has_value(); }
  T& value() noexcept { return *value_; }
 private:
  await_status status_ = await_status::stopped;
  std::optional<T> value_;
};

template <>
class await_result<void> {
 public:
  static await_result value() { await_result r; r.status_ = await_status::value; return r; }
  static await_result error(std::error_code) { await_result r; r.status_ = await_status::error; return r; }
  static await_result stopped() { await_result r; r.status_ = await_status::stopped; return r; }
  bool has_value() const noexcept { return status_ == await_status::value; }
  explicit operator bool() const noexcept { return has_value(); }
 private:
  await_status status_ = await_status::stopped;
};

template <class List> struct sender_value_type;
template <class T> struct sender_value_type<bexec::type_list<std::tuple<std::error_code, T>>> { using type = T; };
template <> struct sender_value_type<bexec::type_list<std::tuple<std::error_code>>> { using type = void; };
template <class T> struct sender_value_type<bexec::type_list<std::tuple<T>>> { using type = T; };
template <> struct sender_value_type<bexec::type_list<std::tuple<>>> { using type = void; };

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
    explicit receiver(sender_awaiter& a) noexcept : awaiter_(&a) {}
    bexec::empty_env get_env() const noexcept { return {}; }

    void set_value(std::error_code ec) noexcept requires std::is_void_v<value_type> {
      awaiter_->result_ = ec ? result_type::error(ec) : result_type::value();
      awaiter_->resume();
    }

    template <class Value>
      requires(!std::is_void_v<value_type> && std::constructible_from<value_type, Value>)
    void set_value(std::error_code ec, Value&& v) noexcept {
      awaiter_->result_ = ec ? result_type::error(ec) : result_type::value(std::forward<Value>(v));
      awaiter_->resume();
    }

    void set_stopped() noexcept {
      awaiter_->result_ = result_type::stopped();
      awaiter_->resume();
    }
   private:
    sender_awaiter* awaiter_;
  };

  using operation_type = decltype(bexec::connect(std::declval<sender_type&&>(), std::declval<receiver>()));

  explicit sender_awaiter(Sender&& sender)
      : operation_(bexec::connect(std::forward<Sender>(sender), receiver(*this))) {}

  sender_awaiter(const sender_awaiter&) = delete;
  sender_awaiter& operator=(const sender_awaiter&) = delete;
  sender_awaiter(sender_awaiter&&) = delete;
  sender_awaiter& operator=(sender_awaiter&&) = delete;

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> cont) noexcept { continuation_ = cont; bexec::start(operation_); }
  result_type await_resume() noexcept { return std::move(result_); }

 private:
  void resume() noexcept { continuation_.resume(); }
  std::coroutine_handle<> continuation_;
  result_type result_;
  operation_type operation_;
};

template <class Sender>
auto async_result(Sender&& s) { return sender_awaiter<Sender>(std::forward<Sender>(s)); }

// ---- the counting coroutine (one per task, single allocation) ----

detached_task counting_coro(io_context& ctx, unsigned count_to,
                             std::atomic<unsigned>* completed) {
  // Hand off to a worker thread.
  auto result = co_await async_result(ctx.get_post_scheduler().schedule());
  if (!result) { co_return; }

  int wid = find_worker_id();

  // CPU work.
  volatile unsigned acc = 0;
  for (unsigned i = 0; i < count_to; ++i) acc += 1;
  (void)acc;

  if (wid >= 0) {
    g_stats[wid].tasks.fetch_add(1, std::memory_order_relaxed);
    g_stats[wid].total_work.fetch_add(count_to, std::memory_order_relaxed);
  }

  completed->fetch_add(1, std::memory_order_release);
}

// ---- run loop ----

void run_context(io_context& ctx, unsigned workers) {
  std::vector<std::thread> th;
  th.reserve(workers - 1);
  for (unsigned i = 1; i < workers; ++i) {
    th.emplace_back([&ctx] {
      register_worker_thread();
      ctx.run();
    });
  }
  register_worker_thread();
  ctx.run();
  for (auto& t : th) t.join();
}

}  // namespace

int main(int argc, char** argv) {
  const unsigned worker_count =
      static_cast<unsigned>(parse_ulong(argc > 1 ? argv[1] : nullptr, k_default_workers));
  const unsigned task_count =
      static_cast<unsigned>(parse_ulong(argc > 2 ? argv[2] : nullptr, k_default_tasks));

  g_max_workers = std::max(worker_count + 1, k_max_workers);
  g_stats      = new worker_stats[g_max_workers]();
  g_worker_ids = new std::thread::id[g_max_workers]();

  // Pre-generate all workloads (sequence prepared before benchmark starts).
  const auto workloads = generate_workloads(task_count);

  io_context_options opts;
  opts.concurrency_hint = worker_count;
  opts.platform.entries = 256;
  io_context ctx(opts);
  if (!ctx.is_open()) { std::cerr << "ctx unavailable\n"; return 1; }

  std::atomic<unsigned> tasks_completed{0};

  // Seed: launch the first worker_count tasks so every worker picks up
  // at least one immediately.
  unsigned seed_n = std::min(worker_count, task_count);
  for (unsigned i = 0; i < seed_n; ++i)
    start(counting_coro(ctx, workloads[i], &tasks_completed));

  // Producer thread: posts remaining tasks at a steady pace (~50 us
  // between launches) so the shared MPSC queue receives a trickle rather
  // than a single bulk that pop_cpu_all() would grab entirely.
  std::thread producer([&] {
    for (unsigned i = seed_n; i < task_count; ++i) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      start(counting_coro(ctx, workloads[i], &tasks_completed));
    }
    // Spin until all tasks complete, then stop the context.
    while (tasks_completed.load(std::memory_order_acquire) < task_count)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ctx.stop();
  });

  const auto t0 = std::chrono::steady_clock::now();
  run_context(ctx, worker_count);
  const auto t1 = std::chrono::steady_clock::now();

  producer.join();

  double secs = std::chrono::duration<double>(t1 - t0).count();

  // ---- report ----
  std::cout << "workers:      " << worker_count << "\n";
  std::cout << "tasks:        " << task_count << "\n";
  std::cout << "count range:  " << k_min_count << " – " << k_max_count << "\n";
  std::cout << "wall time:    " << std::fixed << std::setprecision(3)
            << secs << " s\n\n";

  std::uint64_t total_t = 0, total_w = 0;
  std::vector<std::uint64_t> per_t(worker_count), per_w(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    per_t[i] = g_stats[i].tasks.load();
    per_w[i] = g_stats[i].total_work.load();
    total_t += per_t[i];
    total_w += per_w[i];
  }

  std::cout << "=== per-worker fairness ===\n";
  std::cout << "worker  tasks       work (sum counts)  %tasks   %work\n";
  for (unsigned i = 0; i < worker_count; ++i) {
    double tp = total_t ? 100.0 * per_t[i] / total_t : 0.0;
    double wp = total_w ? 100.0 * per_w[i] / total_w : 0.0;
    std::cout << std::setw(4) << i << "  "
              << std::setw(10) << per_t[i] << "  "
              << std::setw(18) << per_w[i] << "  "
              << std::fixed << std::setprecision(1)
              << std::setw(6) << tp << "%  "
              << std::setw(6) << wp << "%\n";
  }

  double ideal = 100.0 / worker_count;
  double t_rmsd = 0.0, w_rmsd = 0.0;
  for (unsigned i = 0; i < worker_count; ++i) {
    double tp = total_t ? 100.0 * per_t[i] / total_t : 0.0;
    double wp = total_w ? 100.0 * per_w[i] / total_w : 0.0;
    t_rmsd += (tp - ideal) * (tp - ideal);
    w_rmsd += (wp - ideal) * (wp - ideal);
  }
  t_rmsd = std::sqrt(t_rmsd / worker_count);
  w_rmsd = std::sqrt(w_rmsd / worker_count);

  std::cout << "\nfairness RMSD:  tasks=" << std::fixed
            << std::setprecision(2) << t_rmsd << "pp  work=" << w_rmsd
            << "pp  (0 = perfect, lower is better)\n";
  std::cout << "completed:     " << tasks_completed.load() << " / "
            << task_count << "\n";

  delete[] g_stats;
  delete[] g_worker_ids;
  return 0;
}
