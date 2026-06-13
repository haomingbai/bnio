#include <bupp/bupp.h>
#include <sys/socket.h>

#include <bexec/bexec.hpp>
#include <linux/io_uring.h>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t k_default_port = 8080;
constexpr int k_backlog = 1024;
constexpr std::size_t k_max_request_size = 1024U * 1024U;
constexpr int k_max_consecutive_accept_errors = 5;

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) { stop_requested = 1; }

[[nodiscard]] bool is_space(char ch) noexcept {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

[[nodiscard]] char lower_ascii(char ch) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && is_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_space(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] bool iequals(std::string_view lhs,
                           std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (lower_ascii(lhs[index]) != lower_ascii(rhs[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool token_contains(std::string_view value,
                                  std::string_view token) noexcept {
  while (!value.empty()) {
    const std::size_t comma = value.find(',');
    std::string_view part = trim(value.substr(0, comma));
    if (iequals(part, token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    value.remove_prefix(comma + 1);
  }
  return false;
}

enum class parse_status {
  incomplete,
  ready,
  bad_request,
};

struct parsed_request {
  std::string method;
  std::string target;
  std::string body;
  bool keep_alive = true;
};

[[nodiscard]] parse_status try_parse_request(std::string_view bytes,
                                             parsed_request& request) {
  const std::size_t header_end = bytes.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return bytes.size() > k_max_request_size ? parse_status::bad_request
                                             : parse_status::incomplete;
  }

  const std::size_t first_line_end = bytes.find("\r\n");
  if (first_line_end == std::string_view::npos || first_line_end > header_end) {
    return parse_status::bad_request;
  }

  std::string_view first_line = bytes.substr(0, first_line_end);
  const std::size_t method_end = first_line.find(' ');
  if (method_end == std::string_view::npos) {
    return parse_status::bad_request;
  }
  first_line.remove_prefix(method_end + 1);
  const std::size_t target_end = first_line.find(' ');
  if (target_end == std::string_view::npos) {
    return parse_status::bad_request;
  }

  const std::string_view method = bytes.substr(0, method_end);
  const std::string_view target = first_line.substr(0, target_end);
  const std::string_view version = first_line.substr(target_end + 1);

  std::size_t content_length = 0;
  bool saw_connection_close = false;
  bool saw_connection_keep_alive = false;

  std::size_t line_begin = first_line_end + 2;
  while (line_begin < header_end) {
    const std::size_t line_end = bytes.find("\r\n", line_begin);
    if (line_end == std::string_view::npos || line_end > header_end) {
      return parse_status::bad_request;
    }

    const std::string_view line =
        bytes.substr(line_begin, line_end - line_begin);
    line_begin = line_end + 2;
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }

    const std::string_view name = trim(line.substr(0, colon));
    const std::string_view value = trim(line.substr(colon + 1));

    // Fast path: both headers we care about start with 'c'
    if (!name.empty() && lower_ascii(name[0]) == 'c') {
      if (name.size() == 14 && iequals(name, "content-length")) {
        const char* first = value.data();
        const char* last = value.data() + value.size();
        const auto [ptr, ec] = std::from_chars(first, last, content_length);
        if (ec != std::errc{} || ptr != last) {
          return parse_status::bad_request;
        }
      } else if (name.size() == 10 && iequals(name, "connection")) {
        saw_connection_close = token_contains(value, "close");
        saw_connection_keep_alive = token_contains(value, "keep-alive");
      }
    }
  }

  const std::size_t body_begin = header_end + 4;
  if (content_length > k_max_request_size ||
      body_begin + content_length > k_max_request_size) {
    return parse_status::bad_request;
  }
  if (bytes.size() < body_begin + content_length) {
    return parse_status::incomplete;
  }

  request.method.assign(method);
  request.target.assign(target);
  request.body.assign(bytes.substr(body_begin, content_length));
  request.keep_alive =
      !saw_connection_close &&
      (!iequals(version, "HTTP/1.0") || saw_connection_keep_alive);
  return parse_status::ready;
}

[[nodiscard]] std::string make_response(const parsed_request& request) {
  std::string body = request.body;
  if (body.empty()) {
    body = request.method + " " + request.target + "\n";
  }

  // Pre-built constant prefix to avoid repeated operator+= calls
  static constexpr std::string_view k_prefix =
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ";

  std::string response;
  response.reserve(k_prefix.size() + 32 +
                   (request.keep_alive ? 24 : 16) + body.size());
  response = k_prefix;

  // std::to_chars avoids the allocation that std::to_string would make
  char len_buf[32];
  const auto [ptr, _] =
      std::to_chars(len_buf, len_buf + sizeof(len_buf), body.size());
  response.append(len_buf, ptr - len_buf);

  response += "\r\nConnection: ";
  response += request.keep_alive ? "keep-alive" : "close";
  response += "\r\n\r\n";
  response += body;
  return response;
}

[[nodiscard]] std::string make_bad_request_response() {
  constexpr std::string_view body = "bad request\n";
  std::string response;
  response.reserve(128 + body.size());
  response += "HTTP/1.1 400 Bad Request\r\n";
  response += "Content-Type: text/plain\r\n";
  response += "Content-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: close\r\n\r\n";
  response += body;
  return response;
}

[[nodiscard]] std::string describe_error(const std::error_code& error) {
  return error.message();
}

template <class Error>
[[nodiscard]] std::string describe_error(const Error&) {
  return "unknown error";
}

class server;

class session : public std::enable_shared_from_this<session> {
 public:
  session(server& owner, bupp::tcp_socket socket) noexcept
      : owner_(owner), socket_(std::move(socket)) {}

  void start();
  void stop() noexcept;

 private:
  void start_read();
  void on_read(std::size_t bytes);
  void start_write();
  void close() noexcept;

  template <class Error>
  void on_io_error(const Error& error);

  server& owner_;
  bupp::tcp_socket socket_;
  std::string request_;
  std::string response_;
  bool close_after_write_ = false;
  bool closed_ = false;
};

class server {
 public:
  server(bupp::io_context& context, bupp::tcp_acceptor acceptor) noexcept
      : context_(context),
        acceptor_(std::move(acceptor)),
        signal_timer_(context) {}

  void start() {
    start_accept();
    start_signal_poll();
  }

  [[nodiscard]] bool stopping() const noexcept { return stopping_; }

  [[nodiscard]] bupp::io_context& context() noexcept { return context_; }

  void add_session(std::shared_ptr<session> client) {
    sessions_.insert(std::move(client));
  }

  void remove_session(const session* client) noexcept {
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
      if (iterator->get() == client) {
        iterator = sessions_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  template <class Sender, class ValueFn, class ErrorFn>
  void spawn(Sender&& sender, ValueFn&& value_fn, ErrorFn&& error_fn) {
    cleanup_retired_operations();

    using sender_type = std::remove_cvref_t<Sender>;
    using receiver_type =
        operation_receiver<std::decay_t<ValueFn>, std::decay_t<ErrorFn>>;
    using operation_type = decltype(bexec::connect(
        std::declval<sender_type>(), std::declval<receiver_type>()));

    auto token = std::make_shared<operation_token>();
    token->owner = this;

    auto holder = std::make_unique<operation_holder<operation_type>>(
        std::forward<Sender>(sender),
        receiver_type(*this, token, std::forward<ValueFn>(value_fn),
                      std::forward<ErrorFn>(error_fn)));
    token->holder = holder.get();
    operations_.push_back(std::move(holder));
    ++active_operations_;
    operations_.back()->start();
  }

  void request_shutdown() {
    if (stopping_) {
      return;
    }

    stopping_ = true;
    (void)acceptor_.close();
    (void)signal_timer_.cancel();  // cancel the 200ms poll timer immediately

    std::vector<std::shared_ptr<session>> sessions(sessions_.begin(),
                                                   sessions_.end());
    for (const auto& client : sessions) {
      client->stop();
    }

    if (active_operations_ == 0) {
      (void)context_.stop();
    }
  }

 private:
  struct operation_holder_base {
    operation_holder_base() noexcept = default;
    operation_holder_base(const operation_holder_base&) = delete;
    operation_holder_base& operator=(const operation_holder_base&) = delete;
    virtual ~operation_holder_base() = default;

    virtual void start() noexcept = 0;

    bool retired = false;
  };

  template <class Operation>
  class operation_holder : public operation_holder_base {
   public:
    template <class SenderArg, class ReceiverArg>
    operation_holder(SenderArg&& sender, ReceiverArg&& receiver)
        : operation_(bexec::connect(std::forward<SenderArg>(sender),
                                    std::forward<ReceiverArg>(receiver))) {}

    void start() noexcept override { bexec::start(operation_); }

   private:
    Operation operation_;
  };

  struct operation_token {
    server* owner = nullptr;
    operation_holder_base* holder = nullptr;
  };

  template <class ValueFn, class ErrorFn>
  class operation_receiver {
   public:
    operation_receiver(server& owner, std::shared_ptr<operation_token> token,
                       ValueFn value_fn, ErrorFn error_fn)
        : owner_(&owner),
          token_(std::move(token)),
          value_fn_(std::move(value_fn)),
          error_fn_(std::move(error_fn)) {}

    template <class... Args>
    void set_value(Args&&... args) noexcept {
      complete([this, &args...] { value_fn_(std::forward<Args>(args)...); });
    }

    template <class Error>
    void set_error(Error&& error) noexcept {
      complete([this, &error] { error_fn_(std::forward<Error>(error)); });
    }

    void set_stopped() noexcept {
      complete([] {});
    }

   private:
    template <class Function>
    void complete(Function&& function) noexcept {
      try {
        std::forward<Function>(function)();
      } catch (const std::exception& ex) {
        std::cerr << "http_echo_server: unhandled completion exception: "
                  << ex.what() << '\n';
        owner_->request_shutdown();
      } catch (...) {
        std::cerr << "http_echo_server: unhandled completion exception\n";
        owner_->request_shutdown();
      }

      if (token_ != nullptr && token_->holder != nullptr) {
        token_->holder->retired = true;
      }
      owner_->finish_operation();
    }

    server* owner_;
    std::shared_ptr<operation_token> token_;
    ValueFn value_fn_;
    ErrorFn error_fn_;
  };

  void start_accept() {
    if (stopping_) {
      return;
    }

    spawn(
        context_.async_accept(acceptor_, SOCK_CLOEXEC),
        [this](bupp::tcp_socket socket) {
          if (stopping_) {
            return;
          }

          accept_error_count_ = 0;
          auto client = std::make_shared<session>(*this, std::move(socket));
          add_session(client);
          client->start();
          start_accept();
        },
        [this](const auto& error) {
          if (!stopping_) {
            ++accept_error_count_;
            std::cerr << "http_echo_server: accept failed: "
                      << describe_error(error) << '\n';
            if (accept_error_count_ >= k_max_consecutive_accept_errors) {
              // Persistent error — brief backoff to avoid tight loop (EMFILE,
              // etc.)
              (void)signal_timer_.expires_after(std::chrono::milliseconds(500));
              spawn(
                  signal_timer_.async_wait(),
                  [this]() {
                    accept_error_count_ = 0;
                    start_accept();
                  },
                  [this](const auto&) {
                    accept_error_count_ = 0;
                    start_accept();
                  });
            } else {
              start_accept();
            }
          }
        });
  }

  void start_signal_poll() {
    if (stopping_) {
      return;
    }

    (void)signal_timer_.expires_after(std::chrono::milliseconds(200));
    spawn(
        signal_timer_.async_wait(),
        [this]() {
          if (stop_requested != 0) {
            request_shutdown();
            return;
          }
          start_signal_poll();
        },
        [this](const auto& error) {
          if (!stopping_) {
            std::cerr << "http_echo_server: signal timer failed: "
                      << describe_error(error) << '\n';
            request_shutdown();
          }
        });
  }

  void finish_operation() noexcept {
    if (active_operations_ != 0) {
      --active_operations_;
    }
    if (stopping_ && active_operations_ == 0) {
      (void)context_.stop();
    }
  }

  void cleanup_retired_operations() {
    for (auto iterator = operations_.begin(); iterator != operations_.end();) {
      if ((*iterator)->retired) {
        iterator = operations_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  bupp::io_context& context_;
  bupp::tcp_acceptor acceptor_;
  bupp::steady_timer signal_timer_;
  std::list<std::unique_ptr<operation_holder_base>> operations_;
  std::unordered_set<std::shared_ptr<session>> sessions_;
  std::size_t active_operations_ = 0;
  int accept_error_count_ = 0;
  bool stopping_ = false;
};

void session::start() {
  // Pre-allocate buffers once per connection to avoid malloc/free on every
  // keep-alive request.  clear() in libstdc++ preserves capacity.
  request_.reserve(4096);
  response_.reserve(512);
  start_read();
}

void session::stop() noexcept { (void)socket_.close(); }

void session::start_read() {
  if (closed_ || owner_.stopping()) {
    close();
    return;
  }

  auto self = shared_from_this();
  owner_.spawn(
      owner_.context().async_receive(socket_, bupp::dynamic_buffer(request_),
                                     0),
      [self](std::size_t bytes) { self->on_read(bytes); },
      [self](const auto& error) { self->on_io_error(error); });
}

void session::on_read(std::size_t bytes) {
  if (bytes == 0 || owner_.stopping()) {
    close();
    return;
  }

  parsed_request request;
  switch (try_parse_request(request_, request)) {
    case parse_status::incomplete:
      start_read();
      return;

    case parse_status::bad_request:
      response_ = make_bad_request_response();
      close_after_write_ = true;
      break;

    case parse_status::ready:
      response_ = make_response(request);
      close_after_write_ = !request.keep_alive;
      break;
  }

  start_write();
}

void session::start_write() {
  if (closed_) {
    return;
  }

  auto self = shared_from_this();
  owner_.spawn(
      owner_.context().async_write(socket_, bupp::buffer(response_), MSG_NOSIGNAL),
      [self](std::size_t /*bytes*/) {
        if (self->close_after_write_) {
          self->close();
          return;
        }
        self->request_.clear();
        self->response_.clear();
        self->start_read();
      },
      [self](const auto& error) { self->on_io_error(error); });
}

void session::close() noexcept {
  if (closed_) {
    return;
  }
  closed_ = true;
  (void)socket_.close();
  owner_.remove_session(this);
}

template <class Error>
void session::on_io_error(const Error& error) {
  if (!owner_.stopping()) {
    // Suppress ECONNRESET — normal on client disconnect or during shutdown
    if constexpr (std::is_same_v<std::decay_t<Error>, std::error_code>) {
      if (error.value() == ECONNRESET) {
        close();
        return;
      }
    }
    std::cerr << "http_echo_server: client I/O failed: "
              << describe_error(error) << '\n';
  }
  close();
}

[[nodiscard]] bool parse_port(int argc, char** argv, std::uint16_t& port) {
  if (argc < 2) {
    port = k_default_port;
    return true;
  }

  char* end = nullptr;
  const unsigned long value = std::strtoul(argv[1], &end, 10);
  if (end == argv[1] || *end != '\0' || value == 0 || value > 65535UL) {
    std::cerr << "usage: " << argv[0] << " [port]\n";
    return false;
  }

  port = static_cast<std::uint16_t>(value);
  return true;
}

[[nodiscard]] bool setup_acceptor(bupp::tcp_acceptor& acceptor,
                                  std::uint16_t port) {
  if (const std::error_code ec = acceptor.open(bupp::ip::tcp::v4())) {
    std::cerr << "http_echo_server: open failed: " << ec.message() << '\n';
    return false;
  }
  if (const std::error_code ec = acceptor.set_reuse_address(true)) {
    std::cerr << "http_echo_server: setsockopt failed: " << ec.message()
              << '\n';
    return false;
  }
  if (const std::error_code ec =
          acceptor.bind(bupp::ip::endpoint::loopback_v4(port))) {
    std::cerr << "http_echo_server: bind 127.0.0.1:" << port
              << " failed: " << ec.message() << '\n';
    return false;
  }
  if (const std::error_code ec = acceptor.listen(k_backlog)) {
    std::cerr << "http_echo_server: listen failed: " << ec.message() << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = k_default_port;
  if (!parse_port(argc, argv, port)) {
    return 2;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  bupp::io_context_options opts;
  opts.platform.uring.entries = 1024;
  opts.platform.uring.setup_flags =
      IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
  bupp::io_context context(opts);
  if (!context.is_open()) {
    std::cerr << "http_echo_server: io_context is not available\n";
    return 0;
  }

  bupp::tcp_acceptor acceptor;
  if (!setup_acceptor(acceptor, port)) {
    return 1;
  }

  server echo_server(context, std::move(acceptor));
  echo_server.start();

  std::cout << "http_echo_server: listening on http://127.0.0.1:" << port
            << '\n';
  context.run();
  std::cout << "http_echo_server: stopped\n";
  return 0;
}
