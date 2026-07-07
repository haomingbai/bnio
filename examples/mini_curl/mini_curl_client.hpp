#pragma once
#ifndef BUPP_EXAMPLES_MINI_CURL_CLIENT_HPP_
#define BUPP_EXAMPLES_MINI_CURL_CLIENT_HPP_

#include <bupp/bupp.h>
#include <sys/socket.h>

#include <array>
#include <bexec/bexec.hpp>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mini_curl {

constexpr std::size_t k_max_endpoints = 16;
constexpr std::size_t k_receive_size = 16 * 1024;
constexpr int k_max_redirects = 10;

/**
 * Parsed command-line options for a mini_curl request.
 */
struct request_options {
  /** HTTP method to send. */
  std::string method = "GET";

  /** Remote host name or address. */
  std::string host;

  /** Remote service name or numeric port. */
  std::string service = "80";

  /** Absolute request target sent in the request line. */
  std::string target = "/";

  /** Additional request headers, without trailing CRLF. */
  std::vector<std::string> headers;

  /** Optional request body used by methods such as POST. */
  std::string post_data;

  /** Optional output file path; stdout is used when empty. */
  std::string output_file;

  /** Address family preference used during DNS resolution. */
  bupp::ip::address::version address_version =
      bupp::ip::address::version::unspecified;

  /** Whether to use TLS for the connection. */
  bool use_tls = false;

  /** Whether TLS certificate verification should be disabled. */
  bool insecure = false;

  /** Whether HTTP redirects should be followed. */
  bool follow_redirects = false;

  /** Whether help output was requested. */
  bool help = false;

  /** Whether verbose progress should be written to stderr. */
  bool verbose = false;
};

// ---- URL / header helpers ----

/**
 * Returns true when value begins with prefix.
 */
[[nodiscard]] bool starts_with(std::string_view value,
                               std::string_view prefix) noexcept;

/**
 * Returns true when value contains CR or LF characters.
 */
[[nodiscard]] bool contains_crlf(std::string_view value) noexcept;

/**
 * Normalizes an HTTP request target to an absolute path.
 */
[[nodiscard]] std::string normalized_target(std::string target);

/**
 * Removes a URL fragment from a request target.
 */
[[nodiscard]] std::string remove_fragment(std::string_view target);

/**
 * Parses URL authority text into host and service options.
 */
[[nodiscard]] bool parse_authority(std::string_view authority,
                                   request_options& options,
                                   std::string& error);

/**
 * Parses an HTTP or HTTPS URL into request options.
 */
[[nodiscard]] bool parse_url(std::string_view url, request_options& options,
                             std::string& error);

/**
 * Returns true when a header block already contains the named field.
 */
[[nodiscard]] bool header_contains(std::string_view header,
                                   std::string_view name) noexcept;

/**
 * Builds the value for the Host request header.
 */
[[nodiscard]] std::string host_header_value(const request_options& options);

/**
 * Builds an HTTP/1.1 request from parsed options.
 */
[[nodiscard]] std::string build_request(const request_options& options);

/**
 * Parses the numeric status code from an HTTP status line.
 */
[[nodiscard]] int parse_status_code(std::string_view status_line) noexcept;

/**
 * Extracts the Location header value from a header block.
 */
[[nodiscard]] std::string extract_location(std::string_view headers);

// ---- Async operation lifecycle management ----

/**
 * Type-erased base for operations stored by operation_registry.
 */
struct operation_holder_base {
  /**
   * Destroys a stored operation holder.
   */
  virtual ~operation_holder_base() = default;

  /**
   * Starts the wrapped async operation.
   */
  virtual void start() noexcept = 0;
};

/**
 * Thread-local registry that keeps async operations alive until they complete.
 * Operations are cleaned up by reap() after the event loop stops.
 */
class operation_registry {
 public:
  /**
   * Connects a sender to a receiver, starts it, and keeps the operation alive.
   */
  template <class Sender, class Receiver>
  void spawn(Sender&& sender, Receiver&& receiver) {
    using operation_type = decltype(bexec::connect(std::declval<Sender>(),
                                                   std::declval<Receiver>()));

    struct holder final : operation_holder_base {
      operation_type operation;

      holder(Sender&& s, Receiver&& r)
          : operation(bexec::connect(std::forward<Sender>(s),
                                     std::forward<Receiver>(r))) {}

      void start() noexcept override { bexec::start(operation); }
    };

    auto op = std::make_unique<holder>(std::forward<Sender>(sender),
                                       std::forward<Receiver>(receiver));
    op->start();
    ops_.push_back(std::move(op));
  }

  /**
   * Releases all stored async operations.
   */
  void clear() noexcept { ops_.clear(); }

 private:
  std::vector<std::unique_ptr<operation_holder_base>> ops_;
};

/**
 * Asynchronous HTTP client used by the mini_curl example.
 */
class mini_curl_client : public std::enable_shared_from_this<mini_curl_client> {
 public:
  /**
   * Creates a client bound to an io_context, parsed request options, and
   * operation registry.
   */
  mini_curl_client(bupp::io_context& context, request_options options,
                   operation_registry& registry);

  /**
   * Starts the resolve/connect/handshake/send/receive pipeline.
   */
  void start();

  /**
   * Returns the process-style exit code selected by the client.
   */
  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

 private:
  // ---- Receiver types ----

  struct resolve_receiver;
  struct connect_receiver;
  struct handshake_receiver;
  struct send_receiver;
  struct receive_receiver;
  struct shutdown_receiver;

  // ---- Async flow stages ----

  void resolve();
  void on_resolved(std::size_t count) noexcept;
  void connect_next() noexcept;
  void on_connect_error(std::error_code error) noexcept;
  void on_connected() noexcept;

  // TLS
  void do_handshake() noexcept;
  void on_handshake_complete() noexcept;
  void do_shutdown() noexcept;
  void on_shutdown_complete() noexcept;

  // HTTP
  void send_request() noexcept;
  void send_socket_chunk();
  void send_ssl_chunk();
  void on_request_sent(std::size_t bytes_sent) noexcept;

  // Receive loop
  void receive() noexcept;
  void on_received(std::size_t count) noexcept;
  void on_receive_error(std::error_code error) noexcept;

  // Redirect
  void follow_redirect(std::string location);

  // Output
  void write_output(std::string_view data);

  // Termination
  void fail(std::string_view message) noexcept;
  void fail(std::string_view message, std::error_code error) noexcept;
  void finish(int code) noexcept;

  bupp::io_context& context_;
  operation_registry& registry_;
  request_options options_;
  bupp::tcp_socket socket_;
  bupp::ssl_context ssl_context_{bupp::ssl_context_method::tls_client};
  std::unique_ptr<bupp::ssl_stream<bupp::tcp_socket>> ssl_stream_;
  std::array<bupp::ip::endpoint, k_max_endpoints> endpoints_{};
  std::array<char, k_receive_size> receive_buffer_{};
  std::size_t endpoint_count_ = 0;
  std::size_t endpoint_index_ = 0;
  std::string request_;
  std::string header_buffer_;
  std::ofstream output_stream_;
  std::error_code last_connect_error_;
  std::size_t send_offset_ = 0;
  int exit_code_ = 0;
  int redirect_count_ = 0;
  bool header_complete_ = false;
};

// ============================================================
// Receiver implementations (must be in header for template spawn)
// ============================================================
/** @cond BUPP_DETAIL */

struct mini_curl_client::resolve_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::size_t count) noexcept { client->on_resolved(count); }
  void set_error(std::error_code error) noexcept {
    client->fail("resolve failed", error);
  }
  void set_stopped() noexcept { client->fail("resolve stopped"); }
};

struct mini_curl_client::connect_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value() noexcept { client->on_connected(); }
  void set_error(std::error_code error) noexcept {
    client->on_connect_error(error);
  }
  void set_stopped() noexcept { client->fail("connect stopped"); }
};

struct mini_curl_client::handshake_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value() noexcept { client->on_handshake_complete(); }
  void set_error(std::error_code error) noexcept {
    client->fail("TLS handshake failed", error);
  }
  void set_stopped() noexcept { client->fail("handshake stopped"); }
};

struct mini_curl_client::send_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::size_t bytes_sent) noexcept {
    client->on_request_sent(bytes_sent);
  }
  void set_error(std::error_code error) noexcept {
    client->fail("send failed", error);
  }
  void set_stopped() noexcept { client->fail("send stopped"); }
};

struct mini_curl_client::receive_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::size_t count) noexcept { client->on_received(count); }
  void set_error(std::error_code error) noexcept {
    client->on_receive_error(error);
  }
  void set_stopped() noexcept { client->fail("receive stopped"); }
};

struct mini_curl_client::shutdown_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value() noexcept { client->on_shutdown_complete(); }
  void set_error(std::error_code) noexcept { client->on_shutdown_complete(); }
  void set_stopped() noexcept { client->on_shutdown_complete(); }
};

/** @endcond */

// ============================================================
// mini_curl_client inline implementation
// ============================================================

inline mini_curl_client::mini_curl_client(bupp::io_context& context,
                                          request_options options,
                                          operation_registry& registry)
    : context_(context), registry_(registry), options_(std::move(options)) {
  if (options_.insecure) {
    ssl_context_.set_verify_mode(SSL_VERIFY_NONE);
  }
}

inline void mini_curl_client::start() { resolve(); }

inline void mini_curl_client::resolve() {
  if (options_.verbose) {
    std::cerr << "* Resolving " << options_.host << ":" << options_.service;
    if (options_.use_tls) {
      std::cerr << " (TLS)";
    }
    std::cerr << '\n';
  }

  bupp::dns_query query(options_.host, options_.service);
  query.set_address_version(options_.address_version);
  query.set_transport(bupp::dns_transport::tcp);

  const auto scheduler = context_.get_post_scheduler();
  registry_.spawn(
      scheduler.async_resolve(
          std::move(query),
          bupp::dns_result_view(endpoints_.data(), endpoints_.size())),
      resolve_receiver{shared_from_this()});
}

inline void mini_curl_client::on_resolved(std::size_t count) noexcept {
  endpoint_count_ = count;
  endpoint_index_ = 0;
  if (endpoint_count_ == 0) {
    fail("resolve returned no endpoints");
    return;
  }
  if (options_.verbose) {
    std::cerr << "* Resolved " << endpoint_count_ << " endpoint(s)\n";
  }
  connect_next();
}

inline void mini_curl_client::connect_next() noexcept {
  if (endpoint_index_ >= endpoint_count_) {
    if (last_connect_error_) {
      fail("connect failed", last_connect_error_);
    } else {
      fail("connect failed (no routable endpoints)");
    }
    return;
  }

  (void)socket_.close();
  const bupp::ip::endpoint endpoint = endpoints_[endpoint_index_];
  ++endpoint_index_;

  const bupp::ip::address::version version = endpoint.version();
  std::error_code open_error;
  if (version == bupp::ip::address::version::v4) {
    open_error = socket_.open(bupp::ip::tcp::v4());
  } else if (version == bupp::ip::address::version::v6) {
    open_error = socket_.open(bupp::ip::tcp::v6());
  } else {
    // Record unknown version and try next endpoint
    if (options_.verbose) {
      std::cerr << "* Skipping endpoint with unknown address version\n";
    }
    if (!last_connect_error_) {
      last_connect_error_ =
          std::make_error_code(std::errc::address_family_not_supported);
    }
    connect_next();
    return;
  }

  if (open_error) {
    last_connect_error_ = open_error;
    connect_next();
    return;
  }

  if (options_.verbose) {
    std::cerr << "* Connecting to endpoint " << endpoint_index_ << "/"
              << endpoint_count_ << '\n';
  }

  const auto scheduler = context_.get_post_scheduler();
  registry_.spawn(socket_.async_connect(scheduler, endpoint),
                  connect_receiver{shared_from_this()});
}

inline void mini_curl_client::on_connect_error(std::error_code error) noexcept {
  last_connect_error_ = error;
  connect_next();
}

inline void mini_curl_client::on_connected() noexcept {
  if (options_.verbose) {
    std::cerr << "* Connected";
    if (options_.use_tls) {
      std::cerr << "; starting TLS handshake";
    }
    std::cerr << '\n';
  }

  if (options_.use_tls) {
    do_handshake();
  } else {
    send_request();
  }
}

inline void mini_curl_client::do_handshake() noexcept {
  ssl_stream_ = std::make_unique<bupp::ssl_stream<bupp::tcp_socket>>(
      std::move(socket_), ssl_context_);

  const auto scheduler = context_.get_post_scheduler();
  registry_.spawn(
      ssl_stream_->async_handshake(scheduler, bupp::ssl_handshake_type::client),
      handshake_receiver{shared_from_this()});
}

inline void mini_curl_client::on_handshake_complete() noexcept {
  if (options_.verbose) {
    std::cerr << "* TLS handshake complete\n";
  }
  send_request();
}

inline void mini_curl_client::send_request() noexcept {
  if (options_.verbose) {
    std::cerr << "* Sending " << options_.method << " request to "
              << options_.target << '\n';
  }

  request_ = build_request(options_);
  send_offset_ = 0;

  if (options_.use_tls) {
    send_ssl_chunk();
  } else {
    send_socket_chunk();
  }
}

inline void mini_curl_client::send_socket_chunk() {
  const auto scheduler = context_.get_post_scheduler();
  const std::string_view remaining(request_);
  const std::string_view chunk = remaining.substr(send_offset_);
  registry_.spawn(
      socket_.async_write(scheduler, bupp::buffer(chunk.data(), chunk.size()),
                          MSG_NOSIGNAL),
      send_receiver{shared_from_this()});
}

inline void mini_curl_client::send_ssl_chunk() {
  const auto scheduler = context_.get_post_scheduler();
  const std::string_view remaining(request_);
  const std::string_view chunk = remaining.substr(send_offset_);
  registry_.spawn(
      ssl_stream_->async_write(
          scheduler, bupp::buffer(chunk.data(), chunk.size()), MSG_NOSIGNAL),
      send_receiver{shared_from_this()});
}

inline void mini_curl_client::on_request_sent(std::size_t bytes_sent) noexcept {
  if (bytes_sent == 0) {
    fail("send returned zero bytes");
    return;
  }

  send_offset_ += bytes_sent;
  if (send_offset_ < request_.size()) {
    if (options_.use_tls) {
      send_ssl_chunk();
    } else {
      send_socket_chunk();
    }
    return;
  }

  // Request fully sent; start receiving the response
  header_buffer_.clear();
  header_complete_ = false;
  receive();
}

inline void mini_curl_client::receive() noexcept {
  const auto scheduler = context_.get_post_scheduler();

  if (options_.use_tls) {
    registry_.spawn(
        ssl_stream_->async_read(scheduler, bupp::buffer(receive_buffer_)),
        receive_receiver{shared_from_this()});
  } else {
    registry_.spawn(
        socket_.async_read(scheduler, bupp::buffer(receive_buffer_)),
        receive_receiver{shared_from_this()});
  }
}

inline void mini_curl_client::on_received(std::size_t count) noexcept {
  if (count == 0) {
    // TCP EOF — response complete
    finish(0);
    return;
  }

  std::string_view chunk(receive_buffer_.data(), count);

  if (options_.follow_redirects && !header_complete_) {
    header_buffer_.append(chunk);

    const std::size_t header_end = header_buffer_.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      header_complete_ = true;
      std::string_view header_section =
          std::string_view(header_buffer_).substr(0, header_end);

      // Parse status line (first line of headers)
      const std::size_t first_nl = header_section.find("\r\n");
      std::string_view status_line = first_nl == std::string_view::npos
                                         ? header_section
                                         : header_section.substr(0, first_nl);
      int status = parse_status_code(status_line);

      if (status >= 300 && status < 400 && redirect_count_ < k_max_redirects) {
        std::string location = extract_location(header_section);
        if (!location.empty()) {
          if (options_.verbose) {
            std::cerr << "* Redirect (" << status << ") to: " << location
                      << '\n';
          }
          follow_redirect(std::move(location));
          return;
        }
      }

      // Not a redirect (or max reached) — output buffered headers + body
      write_output(header_buffer_);
    }
    // Header not yet complete — continue receiving
    if (!header_complete_) {
      receive();
      return;
    }
  } else {
    write_output(chunk);
  }

  receive();
}

inline void mini_curl_client::on_receive_error(std::error_code error) noexcept {
  // SSL_ERROR_ZERO_RETURN (clean TLS close) and TCP ECONNRESET after data
  // are both reported as connection_reset.  If we already received response
  // data, treat it as a clean server-initiated close.
  if (error == std::make_error_code(std::errc::connection_reset)) {
    if (options_.verbose) {
      std::cerr << "* Server closed connection\n";
    }
    if (options_.use_tls) {
      do_shutdown();
    } else {
      finish(0);
    }
    return;
  }

  fail("receive failed", error);
}

inline void mini_curl_client::follow_redirect(std::string location) {
  ++redirect_count_;

  // Close current connection cleanly
  if (options_.use_tls) {
    // ssl_stream_ owns the moved-from socket_; destroying it closes the fd
    ssl_stream_.reset();
  }
  // socket_ may be moved-from; close() is a no-op on fd == -1
  (void)socket_.close();

  // Parse the redirect URL
  request_options new_options;
  new_options.method = "GET";  // RFC 7231: change to GET on redirect
  new_options.service = options_.use_tls ? "443" : "80";
  new_options.use_tls = options_.use_tls;

  std::string error;
  if (!parse_url(location, new_options, error)) {
    // Not an absolute URL — treat as relative path
    new_options = options_;
    new_options.method = "GET";
    new_options.post_data.clear();
    new_options.target = normalized_target(location);
  }

  // Carry forward user-specified options
  new_options.headers = options_.headers;
  new_options.verbose = options_.verbose;
  new_options.insecure = options_.insecure;
  new_options.follow_redirects = options_.follow_redirects;
  new_options.output_file = options_.output_file;
  new_options.address_version = options_.address_version;

  options_ = std::move(new_options);

  endpoint_index_ = 0;
  endpoint_count_ = 0;
  last_connect_error_ = std::error_code{};

  if (options_.verbose) {
    std::cerr << "* Redirect #" << redirect_count_ << " to "
              << (options_.use_tls ? "https://" : "http://") << options_.host
              << ":" << options_.service << options_.target << '\n';
  }

  resolve();
}

inline void mini_curl_client::do_shutdown() noexcept {
  if (options_.use_tls && ssl_stream_) {
    if (options_.verbose) {
      std::cerr << "* Shutting down TLS\n";
    }
    const auto scheduler = context_.get_post_scheduler();
    registry_.spawn(ssl_stream_->async_shutdown(scheduler),
                    shutdown_receiver{shared_from_this()});
  } else {
    finish(0);
  }
}

inline void mini_curl_client::on_shutdown_complete() noexcept {
  if (options_.verbose) {
    std::cerr << "* TLS shutdown complete\n";
  }
  finish(0);
}

inline void mini_curl_client::write_output(std::string_view data) {
  if (options_.output_file.empty()) {
    std::cout.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!std::cout) {
      fail("stdout write failed");
      return;
    }
  } else {
    if (!output_stream_.is_open()) {
      output_stream_.open(options_.output_file,
                          std::ios::binary | std::ios::trunc);
      if (!output_stream_) {
        fail("failed to open output file: " + options_.output_file);
        return;
      }
    }
    output_stream_.write(data.data(),
                         static_cast<std::streamsize>(data.size()));
    if (!output_stream_) {
      fail("write to output file failed");
      return;
    }
  }
}

inline void mini_curl_client::fail(std::string_view message) noexcept {
  std::cerr << message << '\n';
  finish(1);
}

inline void mini_curl_client::fail(std::string_view message,
                                   std::error_code error) noexcept {
  std::cerr << message << ": " << error.message() << '\n';
  finish(1);
}

inline void mini_curl_client::finish(int code) noexcept {
  exit_code_ = code;
  if (options_.use_tls) {
    // ssl_stream_ holds the socket; destroying it closes the fd
    ssl_stream_.reset();
  }
  (void)socket_.close();
  if (output_stream_.is_open()) {
    output_stream_.close();
  }
  context_.stop();
}

}  // namespace mini_curl

#endif  // BUPP_EXAMPLES_MINI_CURL_CLIENT_HPP_
