#pragma once
#ifndef BNIO_EXAMPLES_MINI_CURL_CLIENT_RECEIVERS_HPP_
#define BNIO_EXAMPLES_MINI_CURL_CLIENT_RECEIVERS_HPP_

#include <memory>

#include "client.hpp"

namespace mini_curl {

// ============================================================
// Receiver implementations (must be in header for template spawn)
// ============================================================
/** @cond BNIO_DETAIL */

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

}  // namespace mini_curl

#endif  // BNIO_EXAMPLES_MINI_CURL_CLIENT_RECEIVERS_HPP_
