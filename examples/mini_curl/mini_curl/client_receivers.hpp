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

  void set_value(std::error_code ec, std::size_t count) noexcept {
    if (ec) {
      client->fail("resolve failed", ec);
    } else {
      client->on_resolved(count);
    }
  }
  void set_stopped() noexcept { client->fail("resolve stopped"); }
};

struct mini_curl_client::connect_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      client->on_connect_error(ec);
    } else {
      client->on_connected();
    }
  }
  void set_stopped() noexcept { client->fail("connect stopped"); }
};

struct mini_curl_client::handshake_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      client->fail("TLS handshake failed", ec);
    } else {
      client->on_handshake_complete();
    }
  }
  void set_stopped() noexcept { client->fail("handshake stopped"); }
};

struct mini_curl_client::send_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::error_code ec, std::size_t bytes_sent) noexcept {
    if (ec) {
      client->fail("send failed", ec);
    } else {
      client->on_request_sent(bytes_sent);
    }
  }
  void set_stopped() noexcept { client->fail("send stopped"); }
};

struct mini_curl_client::receive_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::error_code ec, std::size_t count) noexcept {
    if (ec) {
      client->on_receive_error(ec);
    } else {
      client->on_received(count);
    }
  }
  void set_stopped() noexcept { client->fail("receive stopped"); }
};

struct mini_curl_client::shutdown_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::error_code) noexcept { client->on_shutdown_complete(); }
  void set_stopped() noexcept { client->on_shutdown_complete(); }
};

struct mini_curl_client::timer_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::error_code ec) noexcept {
    // Timer semantics: successful expiry ec={} -> on_timeout; aborted by a
    // non-token source (io_context::stop(), timer object-API cancellation)
    // ec=operation_canceled -> do nothing
    if (!ec) client->on_timeout();
  }
  void set_stopped() noexcept {
    // Stop-token cancellation — nothing to do
  }
};

struct mini_curl_client::final_receiver {
  std::shared_ptr<mini_curl_client> client;

  void set_value(std::error_code) noexcept { client->do_stop(); }
  void set_stopped() noexcept { client->do_stop(); }
};

/** @endcond */

}  // namespace mini_curl

#endif  // BNIO_EXAMPLES_MINI_CURL_CLIENT_RECEIVERS_HPP_
