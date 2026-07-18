#include <bnio/bnio.h>
#include <sys/socket.h>

#include <array>
#include <bexec/bexec.hpp>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(SOCK_CLOEXEC)
#define SOCK_CLOEXEC 0
#endif

using namespace bnio;
namespace {

constexpr std::uint16_t k_port = 8090;
constexpr int k_backlog = 512;
constexpr std::size_t k_buffer_size = 4096;
constexpr unsigned long k_default_workers = 1;
constexpr unsigned long k_max_workers = 1024;

[[nodiscard]] unsigned long parse_arg(char** argv, int argc, int index,
                                      unsigned long fallback) {
  if (argc <= index) {
    return fallback;
  }

  char* end = nullptr;
  const unsigned long value = std::strtoul(argv[index], &end, 10);
  if (end == argv[index] || *end != '\0' || value == 0) {
    return fallback;
  }
  return value;
}

[[nodiscard]] unsigned parse_workers(char** argv, int argc, int index) {
  const unsigned long value = parse_arg(argv, argc, index, k_default_workers);
  if (value > k_max_workers) {
    return static_cast<unsigned>(k_max_workers);
  }
  return static_cast<unsigned>(value);
}

[[nodiscard]] bool parse_bind_any_v4(char** argv, int argc, int index) {
  if (argc <= index) {
    return false;
  }

  const std::string_view value(argv[index]);
  return value == "0.0.0.0" || value == "*" || value == "any";
}

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
    if (handle) {
      handle.resume();
    }
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

enum class await_status {
  value,
  error,
  stopped,
};

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

    void set_value() noexcept
      requires std::is_void_v<value_type>
    {
      awaiter_->result_ = result_type::value();
      awaiter_->resume();
    }

    template <class Value>
      requires(!std::is_void_v<value_type> &&
               std::constructible_from<value_type, Value>)
    void set_value(Value&& value) noexcept {
      awaiter_->result_ = result_type::value(std::forward<Value>(value));
      awaiter_->resume();
    }

    void set_error(std::error_code error) noexcept {
      awaiter_->result_ = result_type::error(error);
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
      : operation_(
            bexec::connect(std::forward<Sender>(sender), receiver(*this))) {}

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

detached_task echo_session(io_context& ctx, tcp_socket sk) {
  auto scheduler = ctx.get_post_scheduler();
  auto handoff = co_await async_result(scheduler.schedule());
  if (!handoff) {
    (void)sk.close();
    co_return;
  }

  std::array<char, k_buffer_size> buf{};

  while (true) {
    auto read_result =
        co_await async_result(sk.async_read(scheduler, buffer(buf), 0));
    if (!read_result || read_result.value() == 0) {
      break;
    }

    std::size_t send_offset = 0;
    const std::size_t send_size = read_result.value();
    while (send_offset < send_size) {
      auto write_result = co_await async_result(sk.async_write(
          scheduler,
          const_buffer(buf.data() + send_offset, send_size - send_offset),
          MSG_NOSIGNAL));
      if (!write_result || write_result.value() == 0) {
        (void)sk.close();
        co_return;
      }
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
    if (!accept_result) {
      co_return;
    }
    start(echo_session(ctx, accept_result.take_value()));
  }
}

void run_context(io_context& ctx, unsigned worker_count) {
  std::vector<std::thread> workers;
  workers.reserve(worker_count - 1);

  for (unsigned index = 1; index < worker_count; ++index) {
    workers.emplace_back([&ctx] { ctx.run(); });
  }

  ctx.run();

  for (std::thread& worker : workers) {
    worker.join();
  }
}

}  // namespace

int main(int argc, char** argv) {
  const auto port =
      static_cast<std::uint16_t>(parse_arg(argv, argc, 1, k_port));
  const unsigned worker_count = parse_workers(argv, argc, 2);
  const bool bind_any_v4 = parse_bind_any_v4(argv, argc, 3);

  io_context_options opts;
  opts.concurrency_hint = worker_count;
#if defined(BNIO_HAS_IO_CONTEXT_LINUX)
  opts.platform.uring.entries = 1024;
#elif defined(BNIO_HAS_IO_CONTEXT_BSD)
  opts.platform.kqueue.entries = 1024;
#endif
  io_context ctx(opts);
  if (!ctx.is_open()) {
    std::cerr << "ctx unavailable\n";
    return 1;
  }

  tcp_acceptor a;
  if (const std::error_code ec = a.open(ip::tcp::v4())) {
    std::cerr << "open failed: " << ec.message() << '\n';
    return 1;
  }
  if (const std::error_code ec = a.set_reuse_address(true)) {
    std::cerr << "setsockopt failed: " << ec.message() << '\n';
    return 1;
  }
  if (const std::error_code ec =
          a.bind(bind_any_v4 ? ip::endpoint::any_v4(port)
                             : ip::endpoint::loopback_v4(port))) {
    std::cerr << "bind failed: " << ec.message() << '\n';
    return 1;
  }
  if (const std::error_code ec = a.listen(k_backlog)) {
    std::cerr << "listen failed: " << ec.message() << '\n';
    return 1;
  }

  start(accept_loop(ctx, a));

  std::cout << "bnio_raw_echo " << port << " workers " << worker_count
            << " bind " << (bind_any_v4 ? "0.0.0.0" : "127.0.0.1") << std::endl;
  run_context(ctx, worker_count);
}
