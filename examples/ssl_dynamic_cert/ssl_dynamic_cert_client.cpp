// TLS client for the dynamic certificate demo: verifies the presented leaf
// against the embedded CA, checks the SNI hostname, then echoes one message.

#include <bnio/bnio.h>
#include <bnio/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <bexec/bexec.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "certs.inc"

namespace {

using stream_type = bnio::ssl::tcp::stream<bnio::tcp_socket>;

constexpr std::string_view k_payload =
    "hello from bnio dynamic certificate demo\n";
constexpr std::size_t k_payload_size = k_payload.size();
constexpr auto k_watchdog_timeout = std::chrono::seconds(10);

// Minimal type-erased operation holder; operations stay alive until the
// context has stopped and the client state is destroyed.
struct op_base {
  virtual ~op_base() = default;
};

class op_registry {
 public:
  template <class Sender, class Receiver>
  void spawn(Sender&& sender, Receiver&& receiver) {
    using op_type = decltype(bexec::connect(std::declval<Sender>(),
                                            std::declval<Receiver>()));
    struct holder final : op_base {
      op_type op;
      holder(Sender&& s, Receiver&& r)
          : op(bexec::connect(std::forward<Sender>(s),
                              std::forward<Receiver>(r))) {}
    };
    auto operation = std::make_unique<holder>(std::forward<Sender>(sender),
                                              std::forward<Receiver>(receiver));
    holder* const registered = operation.get();
    // Register before start: if push_back throws, the operation has not been
    // submitted to the context yet.
    operations_.push_back(std::move(operation));
    bexec::start(registered->op);
  }

  void clear() noexcept { operations_.clear(); }

 private:
  std::vector<std::unique_ptr<op_base>> operations_;
};

[[nodiscard]] std::unique_ptr<X509, decltype(&X509_free)> parse_certificate(
    const char* pem) noexcept {
  BIO* bio = BIO_new_mem_buf(pem, -1);
  if (bio == nullptr) {
    return {nullptr, X509_free};
  }
  X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return {certificate, X509_free};
}

[[nodiscard]] std::string common_name(X509* certificate) {
  if (certificate == nullptr) {
    return {};
  }
  const X509_NAME* name = X509_get_subject_name(certificate);
  if (name == nullptr) {
    return {};
  }
  const int index = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
  if (index < 0) {
    return {};
  }
  const X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, index);
  if (entry == nullptr) {
    return {};
  }
  const ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
  if (data == nullptr) {
    return {};
  }
  unsigned char* utf8 = nullptr;
  const int length = ASN1_STRING_to_UTF8(&utf8, data);
  if (length < 0) {
    return {};
  }
  std::string text(reinterpret_cast<char*>(utf8),
                   static_cast<std::size_t>(length));
  OPENSSL_free(utf8);
  return text;
}

class client_state {
 public:
  client_state(bnio::io_context& context, bnio::tcp_socket socket,
               std::string host, std::uint16_t port);

  client_state(const client_state&) = delete;
  client_state& operator=(const client_state&) = delete;

  [[nodiscard]] bool valid() const noexcept { return ready_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

  void start();
  void on_timeout() noexcept;

 private:
  void begin_handshake();
  void verify_peer();
  void begin_write();
  void begin_read();
  void check_echo();
  void begin_shutdown();
  void close_and_stop() noexcept;
  void fail(const char* stage, std::error_code ec) noexcept;
  void fail(const char* stage, std::string_view detail) noexcept;

  // Receiver callbacks also run while the context drains; route every stop
  // through this guard so the transition is requested at most once.
  void stop_context() noexcept {
    if (!ctx_.is_stopped()) {
      (void)ctx_.stop();
    }
  }

  bnio::io_context& ctx_;
  bnio::ssl::context ssl_{bnio::ssl::context_method::tls_client};
  std::string host_;
  std::uint16_t port_;
  bnio::tcp_socket socket_;
  std::unique_ptr<stream_type> stream_;
  // Destruction order keeps the operations ahead of the stream and context.
  op_registry operations_;
  std::array<char, k_payload_size> buffer_{};
  std::size_t received_ = 0;
  bool ready_ = false;
  bool ok_ = false;
};

struct watchdog_receiver {
  client_state* client;

  void set_value(std::error_code ec) noexcept {
    if (!ec) {
      client->on_timeout();
    }
  }

  void set_stopped() noexcept {}
};

[[nodiscard]] bool parse_port(const char* text, std::uint16_t& port) {
  try {
    const int value = std::stoi(text);
    if (value <= 0 || value > 65535) {
      return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

client_state::client_state(bnio::io_context& context, bnio::tcp_socket socket,
                           std::string host, std::uint16_t port)
    : ctx_(context),
      host_(std::move(host)),
      port_(port),
      socket_(std::move(socket)) {
  if (!ssl_.valid()) {
    return;
  }
  ssl_.set_verify_mode(SSL_VERIFY_PEER);
  auto ca = parse_certificate(k_ca_certificate_pem);
  if (ca == nullptr) {
    return;
  }
  X509_STORE* store = SSL_CTX_get_cert_store(ssl_.native_handle());
  if (store == nullptr || X509_STORE_add_cert(store, ca.get()) != 1) {
    return;
  }
  ready_ = true;
}

void client_state::start() {
  struct receiver {
    client_state* self;

    void set_value(std::error_code ec) noexcept {
      if (ec) {
        self->fail("connect", ec);
        return;
      }
      self->begin_handshake();
    }

    void set_stopped() noexcept {
      self->fail("connect",
                 std::make_error_code(std::errc::operation_canceled));
    }
  };

  operations_.spawn(
      socket_.async_connect(ctx_.get_post_scheduler(),
                            bnio::ip::endpoint::loopback_v4(port_)),
      receiver{this});
}

void client_state::begin_handshake() {
  stream_ = std::make_unique<stream_type>(std::move(socket_), ssl_);
  if (!stream_->valid()) {
    fail("ssl stream", bnio::ssl::base::make_no_ssl_error());
    return;
  }

  SSL* ssl = stream_->native_handle().native_handle();
  bnio::ssl::base::clear_errors();
  if (SSL_set_tlsext_host_name(ssl, host_.c_str()) != 1) {
    fail("sni", bnio::ssl::base::last_error());
    return;
  }

  struct receiver {
    client_state* self;

    void set_value(std::error_code ec) noexcept {
      if (ec) {
        self->fail("handshake", ec);
        return;
      }
      self->verify_peer();
    }

    void set_stopped() noexcept {
      self->fail("handshake",
                 std::make_error_code(std::errc::operation_canceled));
    }
  };

  operations_.spawn(stream_->async_handshake(ctx_.get_post_scheduler(),
                                             bnio::ssl::handshake_type::client),
                    receiver{this});
}

void client_state::verify_peer() {
  SSL* ssl = stream_->native_handle().native_handle();
  std::unique_ptr<X509, decltype(&X509_free)> peer(
      SSL_get1_peer_certificate(ssl), X509_free);
  if (peer == nullptr) {
    fail("peer certificate", bnio::ssl::base::make_no_ssl_error());
    return;
  }

  const long verify = SSL_get_verify_result(ssl);
  const int host_match =
      X509_check_host(peer.get(), host_.c_str(), host_.size(), 0, nullptr);
  STACK_OF(X509)* chain = SSL_get_peer_cert_chain(ssl);

  std::cout << "peer_cn=" << common_name(peer.get()) << '\n';
  std::cout << "verify="
            << (verify == X509_V_OK ? "ok"
                                    : X509_verify_cert_error_string(verify))
            << '\n';
  std::cout << "host=" << (host_match == 1 ? "ok" : "mismatch") << '\n';
  std::cout << "peer_chain_length="
            << (chain != nullptr ? sk_X509_num(chain) : 0) << '\n';

  if (verify != X509_V_OK) {
    fail("verify", X509_verify_cert_error_string(verify));
    return;
  }
  if (host_match != 1) {
    fail("hostname", "certificate does not cover the requested host");
    return;
  }
  begin_write();
}

void client_state::begin_write() {
  struct receiver {
    client_state* self;

    void set_value(std::error_code ec, std::size_t) noexcept {
      if (ec) {
        self->fail("write", ec);
        return;
      }
      self->begin_read();
    }

    void set_stopped() noexcept {
      self->fail("write", std::make_error_code(std::errc::operation_canceled));
    }
  };

  operations_.spawn(stream_->async_write(
                        ctx_.get_post_scheduler(),
                        bnio::const_buffer(k_payload.data(), k_payload_size)),
                    receiver{this});
}

void client_state::begin_read() {
  if (received_ == k_payload_size) {
    check_echo();
    return;
  }

  struct receiver {
    client_state* self;

    void set_value(std::error_code ec, std::size_t transferred) noexcept {
      if (ec) {
        self->fail("read", ec);
        return;
      }
      if (transferred == 0) {
        self->fail("read", "unexpected EOF");
        return;
      }
      self->received_ += transferred;
      if (self->received_ > k_payload_size) {
        self->fail("read", "echo longer than the sent payload");
        return;
      }
      self->begin_read();
    }

    void set_stopped() noexcept {
      self->fail("read", std::make_error_code(std::errc::operation_canceled));
    }
  };

  operations_.spawn(
      stream_->async_read_some(
          ctx_.get_post_scheduler(),
          bnio::buffer(buffer_.data() + received_, k_payload_size - received_)),
      receiver{this});
}

void client_state::check_echo() {
  if (std::string_view(buffer_.data(), received_) != k_payload) {
    fail("echo", "payload mismatch");
    return;
  }
  ok_ = true;
  std::cout << "echo=ok\n";
  begin_shutdown();
}

void client_state::begin_shutdown() {
  // Send close_notify so the server sees an orderly TLS EOF. Peer teardown
  // errors are irrelevant once the echoed payload has been verified.
  struct receiver {
    client_state* self;

    void set_value(std::error_code) noexcept { self->close_and_stop(); }
    void set_stopped() noexcept { self->close_and_stop(); }
  };

  operations_.spawn(stream_->async_shutdown(ctx_.get_post_scheduler()),
                    receiver{this});
}

void client_state::close_and_stop() noexcept {
  if (stream_) {
    (void)stream_->next_layer().close();
  }
  stop_context();
}

void client_state::fail(const char* stage, std::error_code ec) noexcept {
  std::cerr << "client failed at " << stage << ": " << ec.message() << '\n';
  ok_ = false;
  if (stream_) {
    (void)stream_->next_layer().close();
  }
  stop_context();
}

void client_state::fail(const char* stage, std::string_view detail) noexcept {
  std::cerr << "client failed at " << stage << ": " << detail << '\n';
  ok_ = false;
  if (stream_) {
    (void)stream_->next_layer().close();
  }
  stop_context();
}

void client_state::on_timeout() noexcept {
  if (ctx_.is_stopped()) {
    return;
  }
  std::cerr << "watchdog: no completed exchange within 10s\n";
  ok_ = false;
  if (stream_) {
    (void)stream_->next_layer().close();
  }
  stop_context();
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = 9443;
  if (argc > 1 && !parse_port(argv[1], port)) {
    std::cerr << "invalid port: " << argv[1] << '\n';
    return 1;
  }
  const std::string host = argc > 2 ? argv[2] : "a.example.test";

  bnio::io_context ctx;
  if (!ctx.is_open()) {
    std::cerr << "context unavailable\n";
    return 1;
  }

  bnio::tcp_socket socket;
  const std::error_code ec = socket.open(bnio::ip::tcp::v4());
  if (ec) {
    std::cerr << "socket: " << ec.message() << '\n';
    return 1;
  }

  client_state client(ctx, std::move(socket), host, port);
  if (!client.valid()) {
    std::cerr << "failed to load the embedded CA certificate\n";
    return 1;
  }
  std::cout << "connecting to 127.0.0.1:" << port << " with SNI " << host
            << '\n';

  bnio::steady_timer watchdog(ctx, k_watchdog_timeout);
  auto watchdog_operation =
      bexec::connect(watchdog.async_wait(), watchdog_receiver{&client});
  bexec::start(watchdog_operation);

  client.start();
  (void)ctx.run();
  return client.ok() ? 0 : 1;
}
