#include <array>
#include <asio.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "http_message.hpp"

namespace {

using asio::ip::tcp;
using asio_echo::make_bad_request_response;
using asio_echo::make_response;
using asio_echo::parse_status;
using asio_echo::parsed_request;
using asio_echo::try_parse_request;

constexpr std::uint16_t k_default_port = 8082;
constexpr int k_backlog = 1024;

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
// This mirrors bnio::io_context's semantics (drain in-flight work before
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
