#pragma once
#ifndef BNIO_EXAMPLES_MINI_CURL_CLIENT_TRANSFER_HPP_
#define BNIO_EXAMPLES_MINI_CURL_CLIENT_TRANSFER_HPP_

#include <sys/socket.h>

#include <iostream>
#include <string_view>

#include "client_receivers.hpp"

namespace mini_curl {

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
      socket_.async_write(scheduler, bnio::buffer(chunk.data(), chunk.size()),
                          MSG_NOSIGNAL),
      send_receiver{shared_from_this()});
}

inline void mini_curl_client::send_ssl_chunk() {
  const auto scheduler = context_.get_post_scheduler();
  const std::string_view remaining(request_);
  const std::string_view chunk = remaining.substr(send_offset_);
  registry_.spawn(
      ssl_stream_->async_write(
          scheduler, bnio::buffer(chunk.data(), chunk.size()), MSG_NOSIGNAL),
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
        ssl_stream_->async_read(scheduler, bnio::buffer(receive_buffer_)),
        receive_receiver{shared_from_this()});
  } else {
    registry_.spawn(
        socket_.async_read(scheduler, bnio::buffer(receive_buffer_)),
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

}  // namespace mini_curl

#endif  // BNIO_EXAMPLES_MINI_CURL_CLIENT_TRANSFER_HPP_
