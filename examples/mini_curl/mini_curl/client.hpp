#pragma once
#ifndef BNIO_EXAMPLES_MINI_CURL_CLIENT_CLASS_HPP_
#define BNIO_EXAMPLES_MINI_CURL_CLIENT_CLASS_HPP_

#include <bnio/bnio.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "operation_registry.hpp"
#include "request.hpp"

namespace mini_curl {

constexpr std::size_t k_max_endpoints = 16;
constexpr std::size_t k_receive_size = 16 * 1024;
constexpr int k_max_redirects = 10;

/**
 * Asynchronous HTTP client used by the mini_curl example.
 */
class mini_curl_client : public std::enable_shared_from_this<mini_curl_client> {
 public:
  /**
   * Creates a client bound to an io_context, parsed request options, and
   * operation registry.
   */
  mini_curl_client(bnio::io_context& context, request_options options,
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
  struct timer_receiver;
  struct final_receiver;

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

  // Timeout
  void arm_timeout() noexcept;
  void cancel_timeout() noexcept;
  void on_timeout() noexcept;

  // Structured cleanup
  void do_stop() noexcept;

  // Termination
  void fail(std::string_view message) noexcept;
  void fail(std::string_view message, std::error_code error) noexcept;
  void finish(int code) noexcept;

  bnio::io_context& context_;
  operation_registry& registry_;
  request_options options_;
  bnio::tcp_socket socket_;
  bnio::ssl_context ssl_context_{bnio::ssl_context_method::tls_client};
  std::unique_ptr<bnio::ssl_stream<bnio::tcp_socket>> ssl_stream_;
  std::array<bnio::ip::endpoint, k_max_endpoints> endpoints_{};
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
  bool timed_out_ = false;
  bool finished_ = false;
  std::unique_ptr<bnio::steady_timer> timer_;
};

}  // namespace mini_curl

#endif  // BNIO_EXAMPLES_MINI_CURL_CLIENT_CLASS_HPP_
