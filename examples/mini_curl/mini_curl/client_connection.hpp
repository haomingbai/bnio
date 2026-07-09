#pragma once
#ifndef BUPP_EXAMPLES_MINI_CURL_CLIENT_CONNECTION_HPP_
#define BUPP_EXAMPLES_MINI_CURL_CLIENT_CONNECTION_HPP_

#include <iostream>
#include <memory>
#include <utility>

#include "client_receivers.hpp"

namespace mini_curl {

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

}  // namespace mini_curl

#endif  // BUPP_EXAMPLES_MINI_CURL_CLIENT_CONNECTION_HPP_
