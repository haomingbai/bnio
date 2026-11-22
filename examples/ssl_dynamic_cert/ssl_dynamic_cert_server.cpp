// Dynamic TLS certificate selection: the server pauses the handshake in a
// certificate callback, chooses a certificate by SNI, installs it into a
// per-name or shared SSL_CTX, and swaps the SSL_CTX mid-handshake before the
// retry resumes. The mode argument selects the fetch (direct/posted) and
// install (lazy/generic) variants.

#include <bnio/bnio.h>
#include <bnio/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <bexec/bexec.hpp>
#include <bexec/env.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "certs.inc"

namespace {

using stream_type = bnio::ssl::tcp::stream<bnio::tcp_socket>;

constexpr std::size_t k_read_buffer_size = 4096;
constexpr auto k_retry_delay = std::chrono::milliseconds(1);
constexpr auto k_watchdog_timeout = std::chrono::seconds(15);

// Mode bit 0 selects how the factory obtains the certificate; bit 1 selects
// whether the certificate is installed into a fresh context or a shared one.
constexpr unsigned k_posted_fetch = 1U;
constexpr unsigned k_generic_install = 2U;

constexpr const char k_identity_a[] = "a.example.test";
constexpr const char k_identity_b[] = "b.example.test";

struct identity {
  std::unique_ptr<X509, decltype(&X509_free)> leaf{nullptr, X509_free};
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr,
                                                          EVP_PKEY_free};
};

// Minimal type-erased operation holder; operations stay alive until the
// context has stopped and the owning session is destroyed.
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

[[nodiscard]] std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>
parse_private_key(const char* pem) noexcept {
  BIO* bio = BIO_new_mem_buf(pem, -1);
  if (bio == nullptr) {
    return {nullptr, EVP_PKEY_free};
  }
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return {key, EVP_PKEY_free};
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

class session;

class server_state {
 public:
  server_state(bnio::io_context& context, bool fixed_mode, unsigned mode);
  ~server_state();

  server_state(const server_state&) = delete;
  server_state& operator=(const server_state&) = delete;
  server_state(server_state&&) = delete;
  server_state& operator=(server_state&&) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return ca_ != nullptr && identities_.size() == 2 && bootstrap_.valid() &&
           generic_.valid() && reject_.valid();
  }

  [[nodiscard]] bnio::ssl::context& bootstrap() noexcept { return bootstrap_; }

  // Receiver callbacks also run while the context drains; route every stop
  // through this guard so the transition is requested at most once.
  void stop_context() noexcept {
    if (!ctx.is_stopped()) {
      (void)ctx.stop();
    }
  }

  void on_accept(std::error_code ec, bnio::tcp_socket socket) noexcept;

  // Picks the recorded SNI identity and installs it on the connection.
  // Never throws; allocation failures map to errc::not_enough_memory.
  std::error_code select_and_install(stream_type& stream,
                                     unsigned mode) noexcept;

  bnio::io_context& ctx;
  bool timed_out = false;

 private:
  void add_identity(const char* host, const char* certificate_pem,
                    const char* key_pem);
  std::error_code install_identity(bnio::ssl::context& target,
                                   const identity& selected) noexcept;
  std::error_code append_chain(bnio::ssl::context& target) noexcept;
  std::error_code replace_chain(bnio::ssl::context& target) noexcept;
  // These allocate, so exceptions must reach select_and_install's handler.
  std::error_code install_lazy(stream_type& stream, const std::string& host,
                               const identity& selected, unsigned mode);
  std::error_code install_generic(stream_type& stream, const std::string& host,
                                  const identity& selected, unsigned mode);
  std::error_code switch_to_reject(stream_type& stream) noexcept;
  std::error_code reject_after_failure(stream_type& stream,
                                       std::error_code cause) noexcept;
  void report_success(const std::string& host, unsigned mode, X509* leaf) const;

  bnio::ssl::context bootstrap_{bnio::ssl::context_method::tls_server};
  bnio::ssl::context generic_{bnio::ssl::context_method::tls_server};
  bnio::ssl::context reject_{bnio::ssl::context_method::tls_server};
  std::unique_ptr<X509, decltype(&X509_free)> ca_{nullptr, X509_free};
  std::unordered_map<std::string, identity> identities_;
  std::unordered_map<std::string, std::unique_ptr<bnio::ssl::context>>
      lazy_contexts_;
  std::mutex mutex_;
  unsigned counter_ = 0;
  bool fixed_mode_ = false;
  unsigned fixed_mode_value_ = 0;
  std::unordered_map<SSL*, std::string> pending_sni_;
  // Last member: the session owns operations and a stream that reference the
  // contexts above, so it must be destroyed first.
  std::unique_ptr<session> session_;
};

// Connecting the timer operation straight to a receiver nested in
// bnio::ssl::detail makes the timer's unqualified stop_requested() lookup
// ambiguous (that namespace is dragged in by ADL). A namespace-local receiver
// breaks the association; this example forwards no stop token anyway, and
// io_context::stop() still aborts a pending timer wait.
struct timer_sink {
  virtual ~timer_sink() = default;
  virtual void timer_completed(std::error_code ec) noexcept = 0;
  virtual void timer_stopped() noexcept = 0;
};

struct timer_receiver {
  timer_sink* sink;

  void set_value(std::error_code ec) noexcept { sink->timer_completed(ec); }
  void set_stopped() noexcept { sink->timer_stopped(); }
  [[nodiscard]] bexec::empty_env get_env() const noexcept { return {}; }
};

using timer_wait_sender_type =
    decltype(std::declval<bnio::steady_timer&>().async_wait());

class timer_resume_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;

  timer_resume_sender(bnio::steady_timer& timer, server_state& state,
                      stream_type& stream, unsigned mode) noexcept
      : timer_(&timer), state_(&state), stream_(&stream), mode_(mode) {}

  template <class Receiver>
  class operation final : public timer_sink {
   public:
    operation(bnio::steady_timer& timer, server_state& state,
              stream_type& stream, unsigned mode, Receiver receiver)
        : receiver_(std::move(receiver)),
          state_(&state),
          stream_(&stream),
          mode_(mode),
          timer_operation_(
              bexec::connect(timer.async_wait(), timer_receiver{this})) {}

    void start() noexcept { timer_operation_.start(); }

    void timer_completed(std::error_code ec) noexcept override {
      if (ec) {
        bexec::set_value(std::move(receiver_), ec);
        return;
      }
      bexec::set_value(std::move(receiver_),
                       state_->select_and_install(*stream_, mode_));
    }

    void timer_stopped() noexcept override {
      bexec::set_stopped(std::move(receiver_));
    }

   private:
    using timer_operation_type =
        decltype(bexec::connect(std::declval<timer_wait_sender_type>(),
                                std::declval<timer_receiver>()));

    Receiver receiver_;
    server_state* state_;
    stream_type* stream_;
    unsigned mode_;
    timer_operation_type timer_operation_;
  };

  template <class Receiver>
  operation<Receiver> connect(Receiver receiver) && {
    return operation<Receiver>(*timer_, *state_, *stream_, mode_,
                               std::move(receiver));
  }

 private:
  bnio::steady_timer* timer_;
  server_state* state_;
  stream_type* stream_;
  unsigned mode_;
};

// Builds the connection-specific certificate lookup used by the handshake
// factory. It runs synchronously in the retry driver.
struct direct_factory {
  server_state* state;
  stream_type* stream;
  unsigned mode;

  auto operator()(const std::error_code&) const noexcept {
    return bexec::just(state->select_and_install(*stream, mode));
  }
};

// Completes the lookup from a timer callback, after the retry driver parked.
// The timer belongs to the session and outlives the operation holding this
// factory (session member order: timer before the operation registry).
struct posted_factory {
  server_state* state;
  stream_type* stream;
  bnio::steady_timer* timer;
  unsigned mode;

  auto operator()(const std::error_code&) const {
    (void)timer->expires_after(k_retry_delay);
    return timer_resume_sender(*timer, *state, *stream, mode);
  }
};

class session {
 public:
  session(server_state& state, bnio::tcp_socket socket, unsigned mode)
      : state_(state),
        stream_(std::move(socket), state.bootstrap()),
        timer_(state.ctx),
        mode_(mode) {}

  void start();

 private:
  struct handshake_receiver {
    session* self;

    void set_value(std::error_code ec) noexcept {
      if (ec) {
        self->fail(ec);
        return;
      }
      self->do_read();
    }

    void set_stopped() noexcept {
      self->fail(std::make_error_code(std::errc::operation_canceled));
    }
  };

  void do_read();
  void do_write();
  void finish() noexcept;
  void fail(std::error_code ec) noexcept;

  server_state& state_;
  // Declaration order keeps the operation registry ahead of the stream and
  // timer during destruction.
  stream_type stream_;
  bnio::steady_timer timer_;
  op_registry operations_;
  std::array<char, k_read_buffer_size> buffer_{};
  std::size_t pending_write_ = 0;
  unsigned mode_;
};

struct accept_receiver {
  server_state* state;

  void set_value(std::error_code ec, bnio::tcp_socket socket) noexcept {
    state->on_accept(ec, std::move(socket));
  }

  void set_stopped() noexcept { state->stop_context(); }
};

struct watchdog_receiver {
  server_state* state;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      return;
    }
    std::cerr << "watchdog: no completed connection within 15s\n";
    state->timed_out = true;
    state->stop_context();
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

// --- server_state ---

server_state::server_state(bnio::io_context& context, bool fixed_mode,
                           unsigned mode)
    : ctx(context), fixed_mode_(fixed_mode), fixed_mode_value_(mode) {
  ca_ = parse_certificate(k_ca_certificate_pem);
  add_identity(k_identity_a, k_a_certificate_pem, k_a_private_key_pem);
  add_identity(k_identity_b, k_b_certificate_pem, k_b_private_key_pem);
  if (!valid()) {
    return;
  }

  // Tickets can resume a session without invoking the certificate callback.
  bootstrap_.set_options(SSL_OP_NO_TICKET);
  bootstrap_.set_session_cache_mode(SSL_SESS_CACHE_OFF);
  // Record the name for the factory, then pause: the connection has no
  // identity and must not complete this certificate callback yet.
  bootstrap_.set_cert_callback(
      [this](bnio::ssl::context_view, bnio::ssl::stream_view stream,
             bnio::ssl::cert_out&) -> bnio::ssl::cert_result {
        const char* name = stream.servername();
        const std::lock_guard lock(mutex_);
        pending_sni_[stream.native_handle()] = name != nullptr ? name : "";
        return bnio::ssl::cert_result::retry;
      });
  reject_.set_cert_callback([](bnio::ssl::context_view, bnio::ssl::stream_view,
                               bnio::ssl::cert_out&) -> bnio::ssl::cert_result {
    return bnio::ssl::cert_result::fail;
  });
}

server_state::~server_state() = default;

void server_state::add_identity(const char* host, const char* certificate_pem,
                                const char* key_pem) {
  identity parsed;
  parsed.leaf = parse_certificate(certificate_pem);
  parsed.key = parse_private_key(key_pem);
  if (parsed.leaf != nullptr && parsed.key != nullptr) {
    identities_.try_emplace(host, std::move(parsed));
  }
}

std::error_code server_state::install_identity(
    bnio::ssl::context& target, const identity& selected) noexcept {
  std::error_code error = target.use_certificate(selected.leaf.get());
  if (error) {
    return error;
  }
  error = target.use_private_key(selected.key.get());
  if (error) {
    return error;
  }
  return append_chain(target);
}

std::error_code server_state::append_chain(
    bnio::ssl::context& target) noexcept {
  bnio::ssl::base::clear_errors();
  if (SSL_CTX_add1_chain_cert(target.native_handle(), ca_.get()) != 1) {
    return bnio::ssl::base::last_error();
  }
  return {};
}

std::error_code server_state::replace_chain(
    bnio::ssl::context& target) noexcept {
  bnio::ssl::base::clear_errors();
  if (SSL_CTX_clear_chain_certs(target.native_handle()) != 1) {
    return bnio::ssl::base::last_error();
  }
  return append_chain(target);
}

std::error_code server_state::switch_to_reject(stream_type& stream) noexcept {
  // On success the resumed handshake runs reject_'s callback, which fails and
  // lets OpenSSL send the alert.
  return stream.set_context(reject_);
}

std::error_code server_state::reject_after_failure(
    stream_type& stream, std::error_code cause) noexcept {
  const std::error_code reject_error = switch_to_reject(stream);
  if (reject_error) {
    return cause ? cause : reject_error;
  }
  return {};
}

void server_state::report_success(const std::string& host, unsigned mode,
                                  X509* leaf) const {
  std::cout << "handshake sni=" << host << " mode=" << mode << " fetch="
            << (((mode & k_posted_fetch) != 0) ? "posted" : "direct")
            << " install="
            << (((mode & k_generic_install) != 0) ? "generic" : "lazy")
            << " cn=" << common_name(leaf) << '\n';
}

std::error_code server_state::install_lazy(stream_type& stream,
                                           const std::string& host,
                                           const identity& selected,
                                           unsigned mode) {
  std::error_code load_error;
  bnio::ssl::context* target = nullptr;
  {
    const std::lock_guard lock(mutex_);
    auto& slot = lazy_contexts_[host];
    if (!slot) {
      auto fresh = std::make_unique<bnio::ssl::context>(
          bnio::ssl::context_method::tls_server);
      load_error = install_identity(*fresh, selected);
      if (!load_error) {
        slot = std::move(fresh);
      }
    }
    if (slot) {
      target = slot.get();
    }
  }
  if (load_error) {
    return reject_after_failure(stream, load_error);
  }
  // The context is permanently owned by lazy_contexts_, so the switch may
  // happen after the lock is released.
  const std::error_code switch_error = stream.set_context(*target);
  if (switch_error) {
    return reject_after_failure(stream, switch_error);
  }
  report_success(host, mode, selected.leaf.get());
  return {};
}

std::error_code server_state::install_generic(stream_type& stream,
                                              const std::string& host,
                                              const identity& selected,
                                              unsigned mode) {
  // Loading and switching share the lock: a concurrent connection must never
  // observe generic_ holding another name's identity.
  const std::lock_guard lock(mutex_);
  std::error_code load_error = generic_.use_certificate(selected.leaf.get());
  if (!load_error) {
    load_error = generic_.use_private_key(selected.key.get());
  }
  if (!load_error) {
    load_error = replace_chain(generic_);
  }
  if (!load_error) {
    load_error = stream.set_context(generic_);
  }
  if (load_error) {
    return reject_after_failure(stream, load_error);
  }
  report_success(host, mode, selected.leaf.get());
  return {};
}

std::error_code server_state::select_and_install(stream_type& stream,
                                                 unsigned mode) noexcept {
  try {
    std::string host;
    {
      const std::lock_guard lock(mutex_);
      auto position = pending_sni_.find(stream.native_handle().native_handle());
      if (position == pending_sni_.end() || position->second.empty()) {
        return switch_to_reject(stream);
      }
      host = std::move(position->second);
      pending_sni_.erase(position);
    }

    // identities_ is immutable after construction; no lock is needed.
    const auto position = identities_.find(host);
    if (position == identities_.end()) {
      std::cout << "sni=" << host << " -> reject (unknown name)\n";
      return switch_to_reject(stream);
    }
    const identity& selected = position->second;
    if ((mode & k_generic_install) != 0) {
      return install_generic(stream, host, selected, mode);
    }
    return install_lazy(stream, host, selected, mode);
  } catch (const std::bad_alloc&) {
    return std::make_error_code(std::errc::not_enough_memory);
  } catch (...) {
    return std::make_error_code(std::errc::not_enough_memory);
  }
}

void server_state::on_accept(std::error_code ec,
                             bnio::tcp_socket socket) noexcept {
  if (ec) {
    if (!ctx.is_stopped()) {
      std::cerr << "accept: " << ec.message() << '\n';
    }
    stop_context();
    return;
  }

  unsigned mode = fixed_mode_value_;
  if (!fixed_mode_) {
    const std::lock_guard lock(mutex_);
    mode = counter_++ & 3U;
  }
  std::cout << "accepted: mode=" << mode << '\n';
  try {
    auto accepted = std::make_unique<session>(*this, std::move(socket), mode);
    session_ = std::move(accepted);
    session_->start();
  } catch (const std::bad_alloc&) {
    std::cerr << "session allocation failed\n";
    stop_context();
  }
}

// --- session ---

void session::start() {
  auto scheduler = state_.ctx.get_post_scheduler();
  if ((mode_ & k_posted_fetch) == 0) {
    operations_.spawn(
        stream_.async_handshake(scheduler, bnio::ssl::handshake_type::server,
                                direct_factory{&state_, &stream_, mode_}),
        handshake_receiver{this});
  } else {
    operations_.spawn(stream_.async_handshake(
                          scheduler, bnio::ssl::handshake_type::server,
                          posted_factory{&state_, &stream_, &timer_, mode_}),
                      handshake_receiver{this});
  }
}

void session::do_read() {
  struct receiver {
    session* self;

    void set_value(std::error_code ec, std::size_t transferred) noexcept {
      if (ec) {
        self->fail(ec);
        return;
      }
      if (transferred == 0) {
        self->finish();
        return;
      }
      self->pending_write_ = transferred;
      self->do_write();
    }

    void set_stopped() noexcept { self->finish(); }
  };

  operations_.spawn(stream_.async_read_some(state_.ctx.get_post_scheduler(),
                                            bnio::buffer(buffer_)),
                    receiver{this});
}

void session::do_write() {
  struct receiver {
    session* self;

    void set_value(std::error_code ec, std::size_t) noexcept {
      if (ec) {
        self->fail(ec);
        return;
      }
      self->do_read();
    }

    void set_stopped() noexcept { self->finish(); }
  };

  operations_.spawn(
      stream_.async_write(state_.ctx.get_post_scheduler(),
                          bnio::const_buffer(buffer_.data(), pending_write_)),
      receiver{this});
}

void session::finish() noexcept {
  (void)stream_.next_layer().close();
  std::cout << "echo session closed\n";
  state_.stop_context();
}

void session::fail(std::error_code ec) noexcept {
  std::cerr << "session error: " << ec.message() << '\n';
  (void)stream_.next_layer().close();
  state_.stop_context();
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = 9443;
  if (argc > 1 && !parse_port(argv[1], port)) {
    std::cerr << "invalid port: " << argv[1] << '\n';
    return 1;
  }

  bool fixed_mode = false;
  unsigned mode = 0;
  if (argc > 2) {
    try {
      const int value = std::stoi(argv[2]);
      if (value < 0 || value > 3) {
        std::cerr << "invalid mode (expected 0..3): " << argv[2] << '\n';
        return 1;
      }
      fixed_mode = true;
      mode = static_cast<unsigned>(value);
    } catch (...) {
      std::cerr << "invalid mode (expected 0..3): " << argv[2] << '\n';
      return 1;
    }
  }

  bnio::io_context ctx;
  if (!ctx.is_open()) {
    std::cerr << "context unavailable\n";
    return 1;
  }

  bnio::tcp_acceptor acceptor;
  std::error_code ec;
  if ((ec = acceptor.open(bnio::ip::tcp::v4())) ||
      (ec = acceptor.set_reuse_address(true)) ||
      (ec = acceptor.bind(bnio::ip::endpoint::any_v4(port))) ||
      (ec = acceptor.listen(16))) {
    std::cerr << "acceptor: " << ec.message() << '\n';
    return 1;
  }

  server_state state(ctx, fixed_mode, mode);
  if (!state.valid()) {
    std::cerr << "failed to parse the embedded PEM identities\n";
    return 1;
  }

  std::cout << "dynamic certificate server listening on 0.0.0.0:" << port
            << '\n';
  auto accept_operation = bexec::connect(
      acceptor.async_accept(ctx.get_post_scheduler()), accept_receiver{&state});
  bexec::start(accept_operation);

  bnio::steady_timer watchdog(ctx, k_watchdog_timeout);
  auto watchdog_operation =
      bexec::connect(watchdog.async_wait(), watchdog_receiver{&state});
  bexec::start(watchdog_operation);

  (void)ctx.run();
  return state.timed_out ? 1 : 0;
}
