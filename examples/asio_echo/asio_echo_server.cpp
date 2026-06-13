#include <array>
#include <asio.hpp>
#include <cctype>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace {

using asio::ip::tcp;

constexpr std::uint16_t k_default_port = 8082;
constexpr int k_backlog = 1024;
constexpr std::size_t k_max_request_size = 1024U * 1024U;

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

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
    const std::string_view part = trim(value.substr(0, comma));
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
    if (iequals(name, "content-length")) {
      const char* first = value.data();
      const char* last = value.data() + value.size();
      const auto [ptr, ec] = std::from_chars(first, last, content_length);
      if (ec != std::errc{} || ptr != last) {
        return parse_status::bad_request;
      }
    } else if (iequals(name, "connection")) {
      saw_connection_close = token_contains(value, "close");
      saw_connection_keep_alive = token_contains(value, "keep-alive");
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

  std::string response;
  response.reserve(128 + body.size());
  response += "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: text/plain\r\n";
  response += "Content-Length: ";
  response += std::to_string(body.size());
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

// ---------------------------------------------------------------------------
// Forward declarations — session and server are mutually dependent
// ---------------------------------------------------------------------------

class server;

// ---------------------------------------------------------------------------
// session — one per connection
//
// Method bodies that reference server members are defined out-of-line after
// the server class definition to break the circular dependency.
// ---------------------------------------------------------------------------

class session : public std::enable_shared_from_this<session> {
 public:
  session(server& owner, tcp::socket socket) noexcept
      : owner_(owner), socket_(std::move(socket)) {}

  void start();
  void close() noexcept;

 private:
  void read_more();
  void on_read(const std::error_code& error, std::size_t bytes);
  void write_more();
  void on_write(const std::error_code& error, std::size_t written);

  server& owner_;
  tcp::socket socket_;
  std::array<char, 4096> input_{};
  std::string request_;
  std::string response_;
  std::size_t write_offset_ = 0;
  bool close_after_write_ = false;
  bool closed_ = false;
};

// ---------------------------------------------------------------------------
// server
//
// Implements graceful shutdown:
// 1. Close the acceptor — stop accepting new connections.
// 2. Close every active session — pending async ops complete with
//    operation_aborted and their handlers call on_session_closed().
// 3. When the last session is gone, ctx_.stop() is called and run() returns.
//
// This mirrors bupp::io_context's semantics (drain in-flight work before
// stopping), unlike a raw asio::io_context::stop() which exits immediately
// and drops pending operations.
// ---------------------------------------------------------------------------

class server {
 public:
  server(asio::io_context& ctx, std::uint16_t port)
      : ctx_(ctx),
        acceptor_(ctx,
                  tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), port)) {
    acceptor_.listen(k_backlog);
  }

  void start();

  [[nodiscard]] bool stopping() const noexcept { return stopping_; }

  void request_shutdown();

  void on_session_closed(const session* s) noexcept;

 private:
  void accept_next();
  void check_stop();

  asio::io_context& ctx_;
  tcp::acceptor acceptor_;
  std::unordered_set<std::shared_ptr<session>> sessions_;
  bool stopping_ = false;
};

// ===================================================================
// session — out-of-line method bodies (server is now complete)
// ===================================================================

void session::start() { read_more(); }

void session::close() noexcept {
  if (closed_) {
    return;
  }
  closed_ = true;
  std::error_code ignored;
  socket_.shutdown(tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);
  owner_.on_session_closed(this);
}

void session::read_more() {
  if (closed_ || owner_.stopping()) {
    close();
    return;
  }

  auto self = shared_from_this();
  socket_.async_read_some(asio::buffer(input_),
                          [self](std::error_code error, std::size_t bytes) {
                            self->on_read(error, bytes);
                          });
}

void session::on_read(const std::error_code& error, std::size_t bytes) {
  if (error || bytes == 0 || owner_.stopping()) {
    close();
    return;
  }

  request_.append(input_.data(), bytes);
  parsed_request request;
  switch (try_parse_request(request_, request)) {
    case parse_status::incomplete:
      read_more();
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

  write_offset_ = 0;
  write_more();
}

void session::write_more() {
  if (closed_) {
    return;
  }

  auto self = shared_from_this();
  const std::size_t remaining = response_.size() - write_offset_;
  asio::async_write(socket_,
                    asio::buffer(response_.data() + write_offset_, remaining),
                    [self](std::error_code error, std::size_t written) {
                      self->on_write(error, written);
                    });
}

void session::on_write(const std::error_code& error, std::size_t written) {
  if (error || written == 0) {
    close();
    return;
  }

  write_offset_ += written;
  if (write_offset_ < response_.size()) {
    write_more();
    return;
  }

  if (close_after_write_) {
    close();
    return;
  }

  // Reset for next request (keep-alive)
  request_.clear();
  response_.clear();
  write_offset_ = 0;
  read_more();
}

// ===================================================================
// server — out-of-line method bodies (session is now complete)
// ===================================================================

void server::start() { accept_next(); }

void server::request_shutdown() {
  if (stopping_) {
    return;
  }
  stopping_ = true;

  // Stop accepting new connections — cancels the pending async_accept.
  std::error_code ignored;
  acceptor_.close(ignored);

  // Close every active session; their I/O handlers will fire with an error
  // and call on_session_closed().
  for (const auto& s : sessions_) {
    s->close();
  }

  // If there were no sessions to begin with, stop immediately.
  check_stop();
}

void server::on_session_closed(const session* s) noexcept {
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
    if (it->get() == s) {
      sessions_.erase(it);
      break;
    }
  }
  check_stop();
}

void server::accept_next() {
  if (stopping_) {
    return;
  }

  acceptor_.async_accept([this](std::error_code error, tcp::socket socket) {
    if (!error) {
      auto s = std::make_shared<session>(*this, std::move(socket));
      sessions_.insert(s);
      s->start();
    }
    // Re-queue accept even on error (e.g. operation_aborted during
    // shutdown) — the stopping_ guard at the top will skip if needed.
    accept_next();
  });
}

void server::check_stop() {
  if (stopping_ && sessions_.empty()) {
    ctx_.stop();
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

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

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = k_default_port;
  if (!parse_port(argc, argv, port)) {
    return 2;
  }

  asio::io_context ctx;
  server echo_server(ctx, port);

  // Register signal handling.  When SIGINT / SIGTERM arrives, initiate the
  // graceful shutdown sequence described in server::request_shutdown().
  asio::signal_set signals(ctx, SIGINT, SIGTERM);
  signals.async_wait([&echo_server](const std::error_code&, int) {
    echo_server.request_shutdown();
  });

  echo_server.start();

  std::cout << "asio_echo_server: listening on http://127.0.0.1:" << port
            << '\n';
  ctx.run();
  std::cout << "asio_echo_server: stopped\n";
  return 0;
}
