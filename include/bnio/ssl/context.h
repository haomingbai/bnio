/**
 * @file context.h
 * @brief RAII SSL_CTX owner with owned callback closures.
 */

#pragma once
#ifndef BNIO_SSL_CONTEXT_H_
#define BNIO_SSL_CONTEXT_H_

#include <bnio/ssl/base.h>
#include <bnio/ssl/callback.h>
#include <bnio/ssl/context_view.h>
#include <bnio/ssl/detail/callback_slot.h>
#include <bnio/ssl/detail/owned_callback.h>
#include <bnio/ssl/stream_view.h>
#include <openssl/ssl.h>

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/**
 * OpenSSL SSL_CTX method selector.
 */
enum class context_method {
  /**
   * Generic TLS method.
   */
  tls,

  /**
   * TLS client method.
   */
  tls_client,

  /**
   * TLS server method.
   */
  tls_server,
};

/**
 * Direction used when starting an SSL/TLS handshake.
 */
enum class handshake_type {
  /**
   * Start a client-side handshake.
   */
  client,

  /**
   * Start a server-side handshake.
   */
  server,
};

/**
 * Returns the error category used for OpenSSL library errors.
 */
using base::openssl_error_category;

/**
 * Creates an error_code in the OpenSSL error category.
 */
using base::make_openssl_error;

/**
 * The error_code reported when an SSL operation's failure path recorded no
 * OpenSSL error (e.g. a handshake on an invalid stream never reaches
 * OpenSSL). Lives in the OpenSSL error category, never collides with a real
 * OpenSSL code, and represents no TLS-level failure.
 */
using base::make_no_ssl_error;

namespace detail {

/**
 * Maps a context_method selector to the OpenSSL SSL_METHOD passed to the
 * context_base constructor.
 */
[[nodiscard]] inline const SSL_METHOD* select_context_method(
    context_method method) noexcept {
  switch (method) {
    case context_method::tls:
      return TLS_method();
    case context_method::tls_client:
      return TLS_client_method();
    case context_method::tls_server:
      return TLS_server_method();
  }
  return TLS_method();
}

/**
 * The lvalue type a bound ALPN argument is forwarded as: a stored
 * reference_wrapper<T> unwraps to T&, any other stored argument is passed as
 * an lvalue of its own type.
 */
template <class T>
struct bound_arg_lvalue {
  /**
   * Plain stored arguments are forwarded as lvalues of their own type.
   */
  using type = T&;
};

/**
 * Unwraps reference_wrapper arguments to the referenced lvalue type.
 */
template <class T>
struct bound_arg_lvalue<std::reference_wrapper<T>> {
  /**
   * The referenced object is forwarded as an lvalue reference.
   */
  using type = T&;
};

/**
 * Convenience alias for the lvalue type a bound argument is forwarded as.
 */
template <class T>
using bound_arg_lvalue_t = typename bound_arg_lvalue<T>::type;

/**
 * After decay, Fn must be invocable as a non-const lvalue with (context_view,
 * stream_view, alpn_out&, std::span<const unsigned char>, BoundArgs...) and
 * return a value convertible to int; otherwise rejected at overload
 * resolution.
 */
template <class Fn, class Ssl, class Out, class... BoundArgs>
concept alpn_callback_invocable =
    requires(Fn& fn, context_view ctx, Ssl ssl, Out& out,
             std::span<const unsigned char> protocols,
             bound_arg_lvalue_t<BoundArgs>... args) {
      {
        std::invoke(fn, ctx, ssl, out, protocols, args...)
      } -> std::convertible_to<int>;
    };

/**
 * After decay, Fn must be invocable as a non-const lvalue with (context_view,
 * stream_view, sni_out&, BoundArgs...) and return a value convertible to int;
 * otherwise rejected at overload resolution.
 */
template <class Fn, class... BoundArgs>
concept servername_callback_invocable =
    requires(Fn& fn, context_view ctx, stream_view ssl, sni_out& out,
             bound_arg_lvalue_t<BoundArgs>... args) {
      { std::invoke(fn, ctx, ssl, out, args...) } -> std::convertible_to<int>;
    };

/**
 * After decay, Fn must be invocable as a non-const lvalue with (context_view,
 * stream_view, alert_out&, BoundArgs...) and return client_hello_result;
 * otherwise rejected at overload resolution.
 */
template <class Fn, class... BoundArgs>
concept client_hello_callback_invocable =
    requires(Fn& fn, context_view ctx, stream_view ssl, alert_out& out,
             bound_arg_lvalue_t<BoundArgs>... args) {
      {
        std::invoke(fn, ctx, ssl, out, args...)
      } -> std::convertible_to<client_hello_result>;
    };

/**
 * After decay, Fn must be invocable as a non-const lvalue with (context_view,
 * stream_view, cert_out&, BoundArgs...) and return cert_result; otherwise
 * rejected at overload resolution.
 */
template <class Fn, class... BoundArgs>
concept cert_callback_invocable =
    requires(Fn& fn, context_view ctx, stream_view ssl, cert_out& out,
             bound_arg_lvalue_t<BoundArgs>... args) {
      {
        std::invoke(fn, ctx, ssl, out, args...)
      } -> std::convertible_to<cert_result>;
    };

}  // namespace detail

/**
 * RAII owner of an SSL_CTX and callback closures; config and queries forward
 * to base::context_base.
 *
 * Destruction clears native registrations, then destroys the closures, then
 * frees the SSL_CTX. ALPN/SNI/client_hello may be cleared on outliving
 * streams; cert callbacks may not (see set_cert_callback). Closures are
 * heap-owned, so moving is safe.
 */
class context {
 public:
  /**
   * Native ALPN selection callback signature re-exported from context_view.
   */
  using alpn_select_cb = base::context_view::alpn_select_cb;

  /**
   * Native SNI servername callback signature re-exported from context_view.
   */
  using servername_cb = base::context_view::servername_cb;

  /**
   * Native client_hello callback signature re-exported from context_view.
   */
  using client_hello_cb = base::context_view::client_hello_cb;

  /**
   * Native certificate callback signature re-exported from context_view.
   */
  using cert_cb = base::context_view::cert_cb;

  /**
   * Creates an SSL context for the selected TLS method.
   */
  explicit context(context_method method = context_method::tls) noexcept
      : base_(detail::select_context_method(method)) {}

  /**
   * Clears registrations and owned closures, then frees the SSL_CTX; see the
   * class note for outliving-stream limits.
   */
  ~context() noexcept { clear_native_callbacks(); }

  /**
   * Disabled: the context owns an SSL_CTX and callback closures.
   */
  context(const context&) = delete;

  /**
   * Disabled: the context owns an SSL_CTX and callback closures.
   */
  context& operator=(const context&) = delete;

  /**
   * Moves SSL_CTX and closure ownership. Native registrations already point
   * at the heap closures, so they need no rebinding.
   */
  context(context&& other) noexcept
      : base_(std::move(other.base_)), slot_(std::move(other.slot_)) {}

  /**
   * Clears this context's registrations, frees its SSL_CTX, then moves from
   * other. Self-assignment is a no-op.
   */
  context& operator=(context&& other) noexcept {
    if (this != &other) {
      clear_native_callbacks();
      base_ = std::move(other.base_);
      slot_ = std::move(other.slot_);
    }
    return *this;
  }

  /**
   * Write-back proxy handed to an ALPN selection callback. Call set() exactly
   * once to select a protocol; leave it untouched to skip ALPN negotiation.
   */
  class alpn_out {
   public:
    /**
     * Internal: wraps OpenSSL's out parameters. User callbacks receive an
     * alpn_out and never construct one directly.
     */
    alpn_out(const unsigned char** out, unsigned char* outlen) noexcept
        : out_(out), outlen_(outlen) {}

    /**
     * Selects the protocol at data/len; the pointers must stay valid for the
     * connection's lifetime (static or connection-lifetime storage).
     */
    void set(const unsigned char* data, unsigned char len) noexcept {
      *out_ = data;
      *outlen_ = len;
    }

   private:
    const unsigned char** out_;
    unsigned char* outlen_;
  };

  /**
   * Installs an ALPN selector: Fn and Args are decay-copied into a
   * context-owned heap closure that replaces any previous one; a throwing
   * construction keeps the previous callback.
   *
   * Allocator is rebound to the closure type and default-constructed; pass it
   * as the first template argument, e.g. set_alpn_callback<MyAlloc>(fn,
   * args...).
   *
   * Callable: int(context_view, stream_view, alpn_out&,
   * std::span<const unsigned char>, BoundArgs...); the returned int passes
   * through to OpenSSL, and exceptions fail closed as
   * SSL_TLSEXT_ERR_ALERT_FATAL. The context must outlive streams whose
   * handshake should invoke it; reference_wrapper arguments must outlive the
   * closure.
   */
  template <class Allocator = std::allocator<std::byte>, class Fn,
            class... Args>
    requires detail::alpn_callback_invocable<std::decay_t<Fn>, stream_view,
                                             alpn_out, std::decay_t<Args>...>
  void set_alpn_callback(Fn&& f, Args&&... args) {
    using closure_type =
        detail::alpn_closure<alpn_out, Allocator, std::decay_t<Fn>,
                             std::decay_t<Args>...>;
    closure_type* const closure = detail::make_closure<closure_type>(
        Allocator{}, std::forward<Fn>(f), std::forward<Args>(args)...);
    // Replace the native registration before destroying the old closure, so a
    // concurrently running handshake can never observe a freed closure.
    view().set_alpn_select_cb(&closure_type::trampoline, closure);
    detail::callback_base* const previous = std::exchange(slot_.alpn, closure);
    if (previous != nullptr) {
      previous->destroy();
    }
  }

  /**
   * Installs an SNI servername selector; binding/ownership rules as
   * set_alpn_callback.
   *
   * Callable: int(context_view, stream_view, sni_out&, BoundArgs...); the
   * returned int passes through, and exceptions fail closed with
   * SSL_TLSEXT_ERR_ALERT_FATAL and SSL_AD_INTERNAL_ERROR when an alert slot
   * exists; out.set_context may switch the SSL_CTX.
   */
  template <class Allocator = std::allocator<std::byte>, class Fn,
            class... Args>
    requires detail::servername_callback_invocable<std::decay_t<Fn>,
                                                   std::decay_t<Args>...>
  void set_servername_callback(Fn&& f, Args&&... args) {
    using closure_type =
        detail::servername_closure<sni_out, Allocator, std::decay_t<Fn>,
                                   std::decay_t<Args>...>;
    closure_type* const closure = detail::make_closure<closure_type>(
        Allocator{}, std::forward<Fn>(f), std::forward<Args>(args)...);
    // Publish the argument before the callback so the new trampoline always
    // receives a closure of its own type; destroy the old closure afterwards.
    view().set_tlsext_servername_arg(closure);
    view().set_tlsext_servername_callback(&closure_type::trampoline);
    detail::callback_base* const previous =
        std::exchange(slot_.servername, closure);
    if (previous != nullptr) {
      previous->destroy();
    }
  }

  /**
   * Installs a client_hello selector; binding/ownership rules as
   * set_alpn_callback.
   *
   * Callable: client_hello_result(context_view, stream_view, alert_out&,
   * BoundArgs...); results map to SSL_CLIENT_HELLO_*, retry pauses for the
   * handshake factory, and exceptions fail closed with SSL_CLIENT_HELLO_ERROR
   * and SSL_AD_INTERNAL_ERROR when an alert slot exists.
   */
  template <class Allocator = std::allocator<std::byte>, class Fn,
            class... Args>
    requires detail::client_hello_callback_invocable<std::decay_t<Fn>,
                                                     std::decay_t<Args>...>
  void set_client_hello_callback(Fn&& f, Args&&... args) {
    using closure_type =
        detail::client_hello_closure<alert_out, Allocator, std::decay_t<Fn>,
                                     std::decay_t<Args>...>;
    closure_type* const closure = detail::make_closure<closure_type>(
        Allocator{}, std::forward<Fn>(f), std::forward<Args>(args)...);
    // Replace the native registration before destroying the old closure, so a
    // concurrently running handshake can never observe a freed closure.
    view().set_client_hello_cb(&closure_type::trampoline, closure);
    detail::callback_base* const previous =
        std::exchange(slot_.client_hello, closure);
    if (previous != nullptr) {
      previous->destroy();
    }
  }

  /**
   * Installs a per-connection certificate selector; binding/ownership rules as
   * set_alpn_callback.
   *
   * Callable: cert_result(context_view, stream_view, cert_out&, BoundArgs...);
   * ok/fail/retry map to 1/0/-1, retry resumes through the handshake factory,
   * and exceptions return 0. OpenSSL copies cert_cb and its argument into each
   * SSL at SSL_new/SSL_set_SSL_CTX, so the context must outlive every stream
   * that may invoke it, and this callback must not be replaced or cleared
   * while one lives (use context_view::native_handle() to manage it manually
   * instead).
   */
  template <class Allocator = std::allocator<std::byte>, class Fn,
            class... Args>
    requires detail::cert_callback_invocable<std::decay_t<Fn>,
                                             std::decay_t<Args>...>
  void set_cert_callback(Fn&& f, Args&&... args) {
    using closure_type =
        detail::cert_closure<cert_out, Allocator, std::decay_t<Fn>,
                             std::decay_t<Args>...>;
    closure_type* const closure = detail::make_closure<closure_type>(
        Allocator{}, std::forward<Fn>(f), std::forward<Args>(args)...);
    // Install before destroying the old closure; OpenSSL copies the callback
    // and argument into every SSL, so previous-generation streams must be gone.
    view().set_cert_cb(&closure_type::trampoline, closure);
    detail::callback_base* const previous = std::exchange(slot_.cert, closure);
    if (previous != nullptr) {
      previous->destroy();
    }
  }

  /**
   * Non-owning view over the owned SSL_CTX, valid only while this context
   * keeps it alive.
   *
   * Stream construction and set_context() take the owning context directly;
   * use view() only for interfaces that need a non-owning handle.
   */
  [[nodiscard]] context_view view() const noexcept {
    return context_view(base_.native_handle());
  }

  /**
   * The owning base layer. Only const is exposed: a mutable reference would
   * allow moving the SSL_CTX out without clearing native registrations. Use
   * view() for configuration and queries and native_handle() for the raw
   * handle.
   */
  [[nodiscard]] const base::context_base& base() const noexcept {
    return base_;
  }

  /**
   * Returns the owned native SSL_CTX pointer, or nullptr when invalid.
   */
  [[nodiscard]] SSL_CTX* native_handle() const noexcept {
    return base_.native_handle();
  }

  /**
   * Returns whether this context owns a native SSL_CTX.
   */
  [[nodiscard]] bool valid() const noexcept { return base_.valid(); }

  /**
   * Loads a certificate chain file into the context.
   */
  [[nodiscard]] std::error_code use_certificate_chain_file(
      const char* path) noexcept {
    return view().use_certificate_chain_file(path);
  }

  /**
   * Loads a PEM private key file into the context.
   */
  [[nodiscard]] std::error_code use_private_key_file(
      const char* path) noexcept {
    return view().use_private_key_file(path);
  }

  /**
   * Checks whether the loaded private key matches the certificate.
   */
  [[nodiscard]] std::error_code check_private_key() noexcept {
    return view().check_private_key();
  }

  /**
   * Sets OpenSSL certificate verification flags for the context.
   */
  void set_verify_mode(int mode) noexcept { view().set_verify_mode(mode); }

  /**
   * Installs a native ALPN selection callback; arg is not owned and must stay
   * alive as long as the SSL_CTX may invoke it. Prefer set_alpn_callback.
   */
  void set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept {
    view().set_alpn_select_cb(cb, arg);
  }

  /**
   * Installs a native SNI servername callback; cb is not owned and must stay
   * valid while the SSL_CTX may invoke it. Prefer set_servername_callback.
   */
  void set_tlsext_servername_callback(servername_cb cb) noexcept {
    view().set_tlsext_servername_callback(cb);
  }

  /**
   * Sets the opaque argument passed to the SNI callback; arg is not owned.
   */
  void set_tlsext_servername_arg(void* arg) noexcept {
    view().set_tlsext_servername_arg(arg);
  }

  /**
   * Installs a native client_hello callback; cb and arg are not owned and
   * must stay valid while the SSL_CTX may invoke them. Prefer
   * set_client_hello_callback.
   */
  void set_client_hello_cb(client_hello_cb cb, void* arg) noexcept {
    view().set_client_hello_cb(cb, arg);
  }

  /**
   * Installs a native certificate callback; cb and arg are not owned and must
   * stay valid while the SSL_CTX may invoke them. Prefer set_cert_callback.
   */
  void set_cert_cb(cert_cb cb, void* arg) noexcept {
    view().set_cert_cb(cb, arg);
  }

  /**
   * Installs an in-memory certificate, mirroring SSL_CTX_use_certificate;
   * OpenSSL increments its refcount and the caller keeps ownership.
   */
  [[nodiscard]] std::error_code use_certificate(X509* cert) noexcept {
    return view().use_certificate(cert);
  }

  /**
   * Installs an in-memory private key, mirroring SSL_CTX_use_PrivateKey;
   * OpenSSL increments its refcount and the caller keeps ownership.
   */
  [[nodiscard]] std::error_code use_private_key(EVP_PKEY* key) noexcept {
    return view().use_private_key(key);
  }

  /**
   * Loads a single certificate file (SSL_FILETYPE_PEM or SSL_FILETYPE_ASN1).
   */
  [[nodiscard]] std::error_code use_certificate_file(const char* path,
                                                     int file_type) noexcept {
    return view().use_certificate_file(path, file_type);
  }

  /**
   * Sets the maximum depth of the certificate chain verification.
   */
  void set_verify_depth(int depth) noexcept { view().set_verify_depth(depth); }

  /** Adds SSL_OP_* flags to the options already set. */
  void set_options(std::uint64_t options) noexcept {
    view().set_options(options);
  }

  /**
   * Returns the current SSL/TLS option flags.
   */
  [[nodiscard]] std::uint64_t get_options() const noexcept {
    return view().get_options();
  }

  /**
   * Sets the cipher list for TLS 1.2 and earlier.
   */
  [[nodiscard]] std::error_code set_cipher_list(const char* lists) noexcept {
    return view().set_cipher_list(lists);
  }

  /**
   * Sets the cipher suites for TLS 1.3.
   */
  [[nodiscard]] std::error_code set_ciphersuites(const char* suites) noexcept {
    return view().set_ciphersuites(suites);
  }

  /**
   * Loads trusted CA certificates from a file and/or directory; either
   * argument may be nullptr.
   */
  [[nodiscard]] std::error_code load_verify_locations(
      const char* ca_file, const char* ca_dir) noexcept {
    return view().load_verify_locations(ca_file, ca_dir);
  }

  /**
   * Loads the default trusted CA locations.
   */
  [[nodiscard]] std::error_code set_default_verify_paths() noexcept {
    return view().set_default_verify_paths();
  }

  /**
   * Sets the minimum protocol version (0 selects the automatic default).
   */
  [[nodiscard]] std::error_code set_min_proto_version(int version) noexcept {
    return view().set_min_proto_version(version);
  }

  /**
   * Sets the maximum protocol version (0 selects the automatic default).
   */
  [[nodiscard]] std::error_code set_max_proto_version(int version) noexcept {
    return view().set_max_proto_version(version);
  }

  /**
   * Sets the session cache mode; the previous mode is ignored.
   */
  void set_session_cache_mode(long mode) noexcept {
    view().set_session_cache_mode(mode);
  }

  /**
   * Sets the session id context used by servers to scope session resumption.
   */
  [[nodiscard]] std::error_code set_session_id_context(
      const unsigned char* sid, std::size_t length) noexcept {
    return view().set_session_id_context(sid, length);
  }

  /**
   * Installs a TLS key material logging callback for SSLKEYLOGFILE-style
   * debugging (e.g. decrypting captures).
   */
  void set_keylog_callback(void (*cb)(const SSL* ssl,
                                      const char* line)) noexcept {
    view().set_keylog_callback(cb);
  }

  /**
   * Sets the client-side ALPN protocol list in OpenSSL wire format: each
   * protocol name prefixed by a single length byte.
   */
  [[nodiscard]] std::error_code set_alpn_protos(const unsigned char* data,
                                                unsigned int length) noexcept {
    return view().set_alpn_protos(data, length);
  }

 private:
  /**
   * Clears ALPN/SNI/client_hello registrations; cert_cb was copied into each
   * SSL at SSL_new/SSL_set_SSL_CTX and cannot be made safe from here.
   */
  void clear_native_callbacks() noexcept {
    if (base_.native_handle() != nullptr) {
      base_.view().set_alpn_select_cb(nullptr, nullptr);
      base_.view().set_keylog_callback(nullptr);
      base_.view().set_tlsext_servername_callback(nullptr);
      base_.view().set_tlsext_servername_arg(nullptr);
      base_.view().set_client_hello_cb(nullptr, nullptr);
      base_.view().set_cert_cb(nullptr, nullptr);
    }
  }

  // slot_ (closures) must be destroyed before base_ (SSL_CTX), after the
  // destructor body clears the native registrations.
  base::context_base base_;
  detail::callback_slot slot_;
};

}  // namespace bnio::ssl

#endif  // BNIO_SSL_CONTEXT_H_
