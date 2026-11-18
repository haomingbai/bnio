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
 * Returns the error_code bnio reports when an SSL operation's failure path
 * recorded no OpenSSL error at all (for example, a handshake on an invalid
 * stream never reaches OpenSSL). The value lives in the OpenSSL error
 * category, never collides with a real OpenSSL error code, and does not
 * represent any TLS-level failure.
 */
using base::make_no_ssl_error;

namespace detail {

/**
 * Translates a context_method selector into the OpenSSL SSL_METHOD passed
 * to the context_base constructor.
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
 * The lvalue type an ALPN callback bound argument is forwarded as on every
 * invocation: a stored std::reference_wrapper<T> unwraps to T&, any other
 * stored (decay-copied) argument is passed as an lvalue of its own type.
 */
template <class T>
struct bound_arg_lvalue {
  /**
   * Plain stored arguments are forwarded as lvalues of their own type.
   */
  using type = T&;
};

/**
 * reference_wrapper arguments unwrap to the referenced object's lvalue type.
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
 * Constraint on the callable accepted by context::set_alpn_callback. After
 * decay, Fn must be invocable as a non-const lvalue with the framework
 * arguments (non-owning context view passed as an lvalue, connection view,
 * alpn_out proxy, wire-format protocol span) followed by the bound arguments
 * forwarded as lvalues, and its result must convert to the int OpenSSL
 * consumes. Callables that cannot be invoked this way are rejected at
 * overload resolution instead of failing deep inside the closure
 * instantiation.
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
 * Constraint on the callable accepted by
 * context::set_servername_callback. After decay, Fn must be invocable as a
 * non-const lvalue with the framework arguments (non-owning context view,
 * connection view, sni_out proxy) followed by the bound arguments forwarded
 * as lvalues, and its result must convert to the int OpenSSL consumes.
 * Callables that cannot be invoked this way are rejected at overload
 * resolution.
 */
template <class Fn, class... BoundArgs>
concept servername_callback_invocable =
    requires(Fn& fn, context_view ctx, stream_view ssl, sni_out& out,
             bound_arg_lvalue_t<BoundArgs>... args) {
      { std::invoke(fn, ctx, ssl, out, args...) } -> std::convertible_to<int>;
    };

/**
 * Constraint on the callable accepted by
 * context::set_client_hello_callback. After decay, Fn must be invocable as a
 * non-const lvalue with the framework arguments (non-owning context view,
 * connection view, alert_out proxy) followed by the bound arguments forwarded
 * as lvalues, and its result must convert to client_hello_result. Callables
 * that cannot be invoked this way are rejected at overload resolution.
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
 * Constraint on the callable accepted by context::set_cert_callback. After
 * decay, Fn must be invocable as a non-const lvalue with the framework
 * arguments (non-owning context view, connection view, cert_out proxy)
 * followed by the bound arguments forwarded as lvalues, and its result must
 * convert to cert_result. Callables that cannot be invoked this way are
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
 * RAII owner for an OpenSSL SSL_CTX object and its owned callback closures.
 *
 * context composes a base::context_base owner with an inline
 * detail::callback_slot. The owner frees the native SSL_CTX; the slot owns
 * heap-allocated closures installed through the set_*_callback methods. The
 * slot is declared after the owner so destruction runs in the safe order:
 * native callback registrations are cleared first, then the slot destroys the
 * closures, and finally the owner frees the SSL_CTX.
 *
 * The non-owning configuration and query surface is forwarded from
 * base::context_base so existing calls such as ctx.set_verify_mode(...) keep
 * working. ssl::tcp::stream construction and set_context() take the owning
 * context directly; view() returns a context_view for interfaces that only
 * carry a non-owning context handle.
 *
 * Moving a context transfers both the native handle and the closure
 * ownership. Native registrations keep pointing at the same heap closure, so
 * moving a context after installing a callback is safe. For callbacks that
 * OpenSSL looks up dynamically through the connection's current SSL_CTX
 * (ALPN, servername, client_hello), a stream may outlive the bnio context:
 * destroying the context clears those registrations and disables the
 * callbacks without leaving the stream with a dangling pointer. A
 * copied-argument callback such as certificate selection follows a stricter
 * rule: OpenSSL copies cert_cb and cert_cb_arg into every SSL at SSL_new and
 * SSL_set_SSL_CTX time, so a context with a certificate callback must outlive
 * every stream that may invoke it. See set_cert_callback for the full
 * contract.
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
   * Clears the native callback registrations, then lets the slot destroy the
   * owned closures and the owner free the native SSL_CTX.
   *
   * For dynamically looked-up callbacks (ALPN, servername, client_hello) this
   * also disables them on streams that outlive this context. A copied
   * certificate callback is only cleared on the SSL_CTX registration;
   * destroying the context while a stream may still invoke a previously
   * copied certificate callback violates the lifetime contract documented on
   * set_cert_callback.
   */
  ~context() noexcept { clear_native_callbacks(); }

  /**
   * Copy construction is disabled because the context owns an SSL_CTX and
   * callback closures.
   */
  context(const context&) = delete;

  /**
   * Copy assignment is disabled because the context owns an SSL_CTX and
   * callback closures.
   */
  context& operator=(const context&) = delete;

  /**
   * Moves SSL_CTX ownership and callback closure ownership from another
   * context. Native callback registrations are not rebound: they already
   * point at the heap closures, which do not move.
   */
  context(context&& other) noexcept
      : base_(std::move(other.base_)), slot_(std::move(other.slot_)) {}

  /**
   * Clears this context's native callback registrations, frees its current
   * SSL_CTX, and moves ownership from another context. Self-assignment is a
   * no-op.
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
   * Write-back proxy handed to an ALPN selection callback installed through
   * set_alpn_callback. Call set() exactly once to select a protocol; leave
   * it untouched to skip ALPN negotiation.
   */
  class alpn_out {
   public:
    /**
     * Internal: wraps OpenSSL's out parameters for the framework trampoline.
     * User callbacks receive an alpn_out and never construct one directly.
     */
    alpn_out(const unsigned char** out, unsigned char* outlen) noexcept
        : out_(out), outlen_(outlen) {}

    /**
     * Selects the protocol at data with length len. Both pointers must stay
     * valid for the lifetime of the SSL connection (pointing into static or
     * connection-lifetime storage).
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
   * Installs an ALPN selection callback with std::thread-style argument
   * binding. Fn and every Args element are decay-copied into a heap-allocated
   * closure state owned by the context; installing replaces and destroys any
   * previously installed closure. If allocation or construction throws, the
   * previously installed callback stays in effect (strong exception
   * guarantee).
   *
   * The user callable is invoked as: int fn(context_view ctx, stream_view ssl,
   * alpn_out& out, std::span<const unsigned char> protocols, BoundArgs...
   * args) where ctx is a non-owning view of the native SSL_CTX driving the
   * connection, ssl is a non-owning view of the connection being handshaken,
   * out writes the selected protocol via set(), and protocols covers
   * OpenSSL's wire-format protocol list. Both views stay valid only while
   * their owning context or connection is alive. The returned int is passed
   * through to OpenSSL (SSL_TLSEXT_ERR_OK, SSL_TLSEXT_ERR_NOACK, ...).
   * Exceptions escaping fn are swallowed and reported as
   * SSL_TLSEXT_ERR_ALERT_FATAL.
   *
   * The registration is stored on the native SSL_CTX and points at the heap
   * closure, so the context may be moved after installing the callback. The
   * context must outlive every stream whose handshake should still invoke the
   * callback: destroying it clears the registration and destroys the closure.
   *
   * std::reference_wrapper arguments are stored as references and forwarded
   * by reference on every invocation (like std::bind); the referenced object
   * must outlive the closure (until reinstall or context destruction).
   *
   * Allocator is a fit-for-purpose allocator type rebound to the closure
   * state; a default-constructed instance is used. To supply one, name it as
   * the first template argument: ctx.set_alpn_callback<MyAlloc>(fn, args...).
   * Allocation and construction may throw (like std::thread construction).
   *
   * Fn is constrained by detail::alpn_callback_invocable: after decay it must
   * be invocable as a non-const lvalue with exactly the arguments described
   * above and its result must convert to int. A callable that cannot be
   * invoked that way is rejected at overload resolution.
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
   * Installs an SNI servername callback with std::thread-style argument
   * binding, using the same ownership and invocation rules as
   * set_alpn_callback: Fn and every Args element are decay-copied into a
   * heap-allocated closure owned by the context, installing replaces and
   * destroys any previously installed servername closure, and allocation or
   * construction failure leaves the previous callback in effect.
   *
   * The user callable is invoked as: int fn(context_view ctx, stream_view
   * ssl, sni_out& out, BoundArgs... args) where ctx is a non-owning view of
   * the native SSL_CTX driving the connection and ssl is a non-owning view of
   * the connection being handshaken. The returned int is passed through to
   * OpenSSL (SSL_TLSEXT_ERR_OK, SSL_TLSEXT_ERR_NOACK,
   * SSL_TLSEXT_ERR_ALERT_FATAL, ...). Exceptions escaping fn are swallowed
   * and fail closed as SSL_TLSEXT_ERR_ALERT_FATAL with
   * SSL_AD_INTERNAL_ERROR recorded when an alert slot exists. The callback
   * may switch the connection's SSL_CTX through out.set_context.
   *
   * std::reference_wrapper arguments are stored as references and forwarded
   * by reference on every invocation. Allocator is rebound to the closure
   * state; a default-constructed instance is used unless supplied as the
   * first template argument.
   *
   * The registration is stored on the native SSL_CTX (callback and opaque
   * argument) and points at the heap closure, so the context may be moved
   * after installing the callback. The context must outlive every stream
   * whose handshake should still invoke the callback: destroying it clears
   * the registration and destroys the closure.
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
    // Publish the argument before the callback: from the moment the new
    // trampoline can run, the argument it receives belongs to a closure of
    // its own type. The old closure is destroyed only after the native
    // registration no longer references it.
    view().set_tlsext_servername_arg(closure);
    view().set_tlsext_servername_callback(&closure_type::trampoline);
    detail::callback_base* const previous =
        std::exchange(slot_.servername, closure);
    if (previous != nullptr) {
      previous->destroy();
    }
  }

  /**
   * Installs a client_hello callback with std::thread-style argument
   * binding, using the same ownership and invocation rules as
   * set_alpn_callback: Fn and every Args element are decay-copied into a
   * heap-allocated closure owned by the context, installing replaces and
   * destroys any previously installed client_hello closure, and allocation or
   * construction failure leaves the previous callback in effect.
   *
   * The user callable is invoked as: client_hello_result fn(context_view ctx,
   * stream_view ssl, alert_out& out, BoundArgs... args). The returned value
   * maps directly to the matching SSL_CLIENT_HELLO_* constant; retry pauses
   * the handshake and is resumed through the factory-driven handshake retry
   * surface. Exceptions escaping fn are swallowed and fail closed as
   * SSL_CLIENT_HELLO_ERROR with SSL_AD_INTERNAL_ERROR recorded when an alert
   * slot exists.
   *
   * std::reference_wrapper arguments are stored as references and forwarded
   * by reference on every invocation. Allocator is rebound to the closure
   * state; a default-constructed instance is used unless supplied as the
   * first template argument.
   *
   * The registration is stored on the native SSL_CTX (callback and opaque
   * argument) and points at the heap closure, so the context may be moved
   * after installing the callback. The context must outlive every stream
   * whose handshake should still invoke the callback: destroying it clears
   * the registration and destroys the closure.
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
   * Installs a per-connection certificate callback with std::thread-style
   * argument binding, using the same ownership and invocation rules as
   * set_alpn_callback: Fn and every Args element are decay-copied into a
   * heap-allocated closure owned by the context, installing replaces and
   * destroys any previously installed certificate closure, and allocation or
   * construction failure leaves the previous callback in effect.
   *
   * The user callable is invoked as: cert_result fn(context_view ctx,
   * stream_view ssl, cert_out& out, BoundArgs... args). ok, fail, and retry
   * map directly to the OpenSSL return values 1, 0, and -1; retry pauses the
   * handshake with SSL_ERROR_WANT_X509_LOOKUP and is resumed through the
   * factory-driven handshake retry surface. Exceptions escaping fn are
   * swallowed and fail closed by returning 0.
   *
   * OpenSSL copies cert_cb and cert_cb_arg into every SSL's per-connection
   * CERT at SSL_new and SSL_set_SSL_CTX time. The owning context therefore
   * MUST outlive every stream that may invoke the certificate callback. The
   * callback must not be replaced or cleared while such a stream is alive:
   * replacing it destroys the previous closure, and a stream that still holds
   * the previous generation's copied argument would then reach freed memory.
   * Clearing the callback through this API has the same restriction.
   *
   * This restriction is deliberate: ex_data-based indirection does not solve
   * the underlying lifetime problem (freeing the indirection at context
   * destruction still leaves per-SSL copies dangling, and not freeing it
   * leaks), so the owning API chooses a strict contract instead. Users who
   * need streams to outlive the context with a certificate callback must use
   * the non-owning context_view::native_handle() / raw set_cert_cb surface
   * and manage the callback and its argument lifetime themselves.
   *
   * The same rule will apply to future copied-argument callbacks (msg,
   * default_passwd, allow_early_data, async, record_padding, CT validation,
   * security, and ctx-level custom extensions); currently only cert_cb is
   * implemented.
   *
   * std::reference_wrapper arguments are stored as references and forwarded
   * by reference on every invocation. Allocator is rebound to the closure
   * state; a default-constructed instance is used unless supplied as the
   * first template argument.
   *
   * The context may be moved after installing the callback: the heap closure
   * does not move, and the closure ledger moves between contexts.
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
    // Install the native registration before destroying the old closure.
    // OpenSSL copies the callback and this argument into every SSL at
    // SSL_new/SSL_set_SSL_CTX time, so the old closure may only be destroyed
    // when no stream from the previous generation is alive; that is part of
    // the documented lifetime contract.
    view().set_cert_cb(&closure_type::trampoline, closure);
    detail::callback_base* const previous = std::exchange(slot_.cert, closure);
    if (previous != nullptr) {
      previous->destroy();
    }
  }

  /**
   * Returns a non-owning view over the owned native SSL_CTX, or an invalid
   * view when this context owns no SSL_CTX. The view is valid only while this
   * context keeps the handle alive.
   *
   * ssl::tcp::stream construction and set_context() accept the owning context
   * directly; use view() only when an interface needs a non-owning handle.
   */
  [[nodiscard]] context_view view() const noexcept {
    return context_view(base_.native_handle());
  }

  /**
   * Returns the owning base layer. Only the const overload is exposed: a
   * mutable reference would allow moving the owning SSL_CTX out of the
   * context without clearing its native callback registrations. Use view()
   * for configuration and queries, and native_handle() for the raw handle.
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
   * Installs a native ALPN selection callback. The context does not own arg;
   * the caller must keep it alive for as long as the native SSL_CTX may
   * invoke the callback. Prefer set_alpn_callback for closures owned by the
   * context.
   */
  void set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept {
    view().set_alpn_select_cb(cb, arg);
  }

  /**
   * Installs a native SNI servername callback. The context does not own cb;
   * the caller must keep it valid for as long as the native SSL_CTX may
   * invoke the callback. Prefer set_servername_callback for closures owned by
   * the context.
   */
  void set_tlsext_servername_callback(servername_cb cb) noexcept {
    view().set_tlsext_servername_callback(cb);
  }

  /**
   * Sets the opaque argument OpenSSL passes to the SNI servername callback.
   * The context does not own arg.
   */
  void set_tlsext_servername_arg(void* arg) noexcept {
    view().set_tlsext_servername_arg(arg);
  }

  /**
   * Installs a native client_hello callback. The context does not own cb or
   * arg; the caller must keep them valid for as long as the native SSL_CTX
   * may invoke the callback. Prefer set_client_hello_callback for closures
   * owned by the context.
   */
  void set_client_hello_cb(client_hello_cb cb, void* arg) noexcept {
    view().set_client_hello_cb(cb, arg);
  }

  /**
   * Installs a native certificate callback. The context does not own cb or
   * arg; the caller must keep them valid for as long as the native SSL_CTX
   * may invoke the callback. Prefer set_cert_callback for closures owned by
   * the context.
   */
  void set_cert_cb(cert_cb cb, void* arg) noexcept {
    view().set_cert_cb(cb, arg);
  }

  /**
   * Installs an in-memory certificate on the context, mirroring
   * SSL_CTX_use_certificate. OpenSSL increments the reference count; the
   * caller keeps ownership of cert.
   */
  [[nodiscard]] std::error_code use_certificate(X509* cert) noexcept {
    return view().use_certificate(cert);
  }

  /**
   * Installs an in-memory private key on the context, mirroring
   * SSL_CTX_use_PrivateKey. OpenSSL increments the reference count; the
   * caller keeps ownership of key.
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

  /**
   * Sets SSL/TLS option flags (SSL_OP_*); the previous options are ignored.
   */
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
   * debugging (e.g. decrypting captures in an external analyzer).
   */
  void set_keylog_callback(void (*cb)(const SSL* ssl,
                                      const char* line)) noexcept {
    view().set_keylog_callback(cb);
  }

  /**
   * Sets the client-side ALPN protocol list in OpenSSL wire format: a
   * sequence of protocol names, each prefixed by a single length byte.
   */
  [[nodiscard]] std::error_code set_alpn_protos(const unsigned char* data,
                                                unsigned int length) noexcept {
    return view().set_alpn_protos(data, length);
  }

 private:
  /**
   * Clears native callback registrations that could otherwise reach closures
   * this context is about to destroy.
   *
   * ALPN, servername, and client_hello registrations are looked up through
   * the connection's current SSL_CTX at invocation time, so clearing them
   * here also safely disables the callbacks on streams that outlive this
   * context.
   *
   * The certificate callback is different: OpenSSL copies its callback
   * pointer and argument into every SSL's per-connection CERT at SSL_new and
   * SSL_set_SSL_CTX time. Those copies are not affected by clearing the
   * SSL_CTX registration, so destroying this context while a stream may still
   * invoke a previously copied certificate callback is a lifetime contract
   * violation. The clear here only prevents the SSL_CTX registration itself
   * from reaching the closure being destroyed; it cannot make live
   * connections safe, and the high-level owning API documents that as
   * unsupported.
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

  // Declared in this order so slot_ (callbacks) is destroyed before base_
  // (native SSL_CTX), after the destructor body has cleared the native
  // registrations.
  base::context_base base_;
  detail::callback_slot slot_;
};

}  // namespace bnio::ssl

#endif  // BNIO_SSL_CONTEXT_H_
