#include <bupp/bupp.h>
#include <sys/socket.h>

#include <array>
#include <bexec/bexec.hpp>
#include <coroutine>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

using namespace bupp;
namespace {

constexpr std::uint16_t k_port = 8090;
constexpr int k_backlog = 512;
constexpr std::size_t k_buffer_size = 4096;

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

  detached_task() noexcept = default;
  explicit detached_task(handle_type handle) noexcept : handle_(handle) {}

  detached_task(const detached_task&) = delete;
  detached_task& operator=(const detached_task&) = delete;

  detached_task(detached_task&& other) noexcept
      : handle_(std::exchange(other.handle_, {})) {}

  detached_task& operator=(detached_task&& other) noexcept {
    if (this != &other) {
      destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

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

  static await_result error(std::error_code error) {
    await_result result;
    result.status_ = await_status::error;
    result.error_ = error;
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

  [[nodiscard]] bool stopped_completion() const noexcept {
    return status_ == await_status::stopped;
  }

  [[nodiscard]] std::error_code error() const noexcept { return error_; }

  T& value() noexcept { return *value_; }
  T&& take_value() noexcept { return std::move(*value_); }

 private:
  await_status status_ = await_status::stopped;
  std::optional<T> value_;
  std::error_code error_;
};

template <class List>
struct sender_value_type;

template <class T>
struct sender_value_type<bexec::type_list<std::tuple<T>>> {
  using type = T;
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

    template <class Value>
      requires std::constructible_from<value_type, Value>
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

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = k_port;
  if (argc > 1) {
    port = static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));
  }

  io_context_options opts;
  opts.platform.uring.entries = 1024;
  opts.platform.uring.setup_flags = IORING_SETUP_COOP_TASKRUN;
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
  if (const std::error_code ec = a.bind(ip::endpoint::loopback_v4(port))) {
    std::cerr << "bind failed: " << ec.message() << '\n';
    return 1;
  }
  if (const std::error_code ec = a.listen(k_backlog)) {
    std::cerr << "listen failed: " << ec.message() << '\n';
    return 1;
  }

  start(accept_loop(ctx, a));

  std::cout << "bupp_raw_echo " << port << std::endl;
  ctx.run();
}
