/**
 * @file owned_callback.h
 * @brief Shared helpers for heap-allocated owned callback closures.
 *
 * Every high-level callback installed through ssl::context follows the same
 * ownership model: the decay-copied callable and its decay-copied bound
 * arguments form one closure allocated on the heap. The closure is referenced
 * directly by the native callback's void* argument, while the owning
 * detail::callback_slot keeps the pointer only as an ownership ledger. The
 * helpers here factor out the parts every channel shares:
 *
 * - callback_closure_base implements allocator-symmetric destruction.
 * - bound_invoker stores the callable and bound arguments and applies the
 *   shared invocation rules (framework arguments first, bound arguments as
 *   lvalues, std::reference_wrapper unwrapping).
 * - make_closure allocates and constructs a closure with the strong exception
 *   guarantee.
 *
 * The concrete closures (ALPN, SNI servername, client_hello, and
 * per-connection certificate) derive from these helpers and add a noexcept
 * static trampoline with the OpenSSL C callback signature. Every channel
 * stores its heap closure pointer directly as the native callback's void*
 * argument; the certificate trampoline therefore reaches its closure in the
 * same way as the others and does not consult any global or ex_data state.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_OWNED_CALLBACK_H_
#define BNIO_SSL_DETAIL_OWNED_CALLBACK_H_

#include <bnio/ssl/context_view.h>
#include <bnio/ssl/detail/callback_slot.h>
#include <bnio/ssl/stream_view.h>
#include <openssl/ssl.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <tuple>
#include <utility>

namespace bnio::ssl::detail {

/**
 * Allocator-aware destruction for a heap-allocated callback closure.
 *
 * Concrete closures derive from callback_closure_base<Concrete, Allocator>.
 * destroy() rebinds the stored allocator to the concrete closure type,
 * destroys the closure through it, and deallocates the storage, keeping
 * allocation and deallocation symmetric even for a stateful allocator.
 *
 * Destruction always runs through callback_base::destroy(); the callback_base
 * destructor stays protected and non-virtual, so a closure is never deleted
 * through a base pointer.
 */
template <class Concrete, class Allocator>
class callback_closure_base : public callback_base {
 public:
  /**
   * Retains the allocator copy used to allocate the closure.
   */
  explicit callback_closure_base(const Allocator& alloc) : allocator_(alloc) {}

  /**
   * Destroys and deallocates the concrete closure object.
   */
  void destroy() noexcept override {
    using closure_alloc_t = typename std::allocator_traits<
        Allocator>::template rebind_alloc<Concrete>;
    closure_alloc_t alloc(allocator_);
    Concrete* const closure = static_cast<Concrete*>(this);
    std::allocator_traits<closure_alloc_t>::destroy(alloc, closure);
    std::allocator_traits<closure_alloc_t>::deallocate(alloc, closure, 1);
  }

 private:
  Allocator allocator_;
};

/**
 * Owned callable plus its bound arguments for one callback closure.
 *
 * Fn and every Args element are stored by value after decay; callers pass the
 * already-decayed types. operator() invokes the callable with the framework
 * arguments followed by every stored argument as an lvalue, like std::bind:
 * plain bound arguments are persistent mutable state shared by all
 * invocations, and a std::reference_wrapper is unwrapped to the referenced
 * object on every invocation.
 */
template <class Fn, class... Args>
class bound_invoker {
 public:
  /**
   * Copies or moves the callable and each bound argument into the closure.
   */
  template <class F, class... As>
  explicit bound_invoker(F&& fn, As&&... args)
      : fn_(std::forward<F>(fn)), args_(std::forward<As>(args)...) {}

  /**
   * Invokes the stored callable with the framework arguments followed by the
   * stored arguments forwarded as lvalues.
   */
  template <class... FrameworkArgs>
  decltype(auto) operator()(FrameworkArgs&&... args) {
    return invoke(std::index_sequence_for<Args...>{},
                  std::forward<FrameworkArgs>(args)...);
  }

 private:
  /**
   * Unwraps a bound std::reference_wrapper binding to the referenced lvalue
   * (std::bind semantics).
   */
  template <class T>
  static constexpr T& unwrap(std::reference_wrapper<T> value) noexcept {
    return value.get();
  }

  /**
   * Passes a plain bound argument through as an lvalue.
   */
  template <class T>
  static constexpr T& unwrap(T& value) noexcept {
    return value;
  }

  template <std::size_t... I, class... FrameworkArgs>
  decltype(auto) invoke(std::index_sequence<I...>, FrameworkArgs&&... args) {
    return std::invoke(fn_, std::forward<FrameworkArgs>(args)...,
                       unwrap(std::get<I>(args_))...);
  }

  Fn fn_;
  std::tuple<Args...> args_;
};

/**
 * Allocates and constructs one closure with the strong exception guarantee.
 *
 * The closure is allocated through Allocator rebound to Closure and
 * constructed with alloc followed by the forwarded callable and bound
 * arguments, mirroring std::thread construction. If construction throws, the
 * storage is deallocated and the exception is rethrown, so an install path
 * can replace a native registration only after the new closure exists.
 */
template <class Closure, class Allocator, class Fn, class... Args>
[[nodiscard]] Closure* make_closure(const Allocator& alloc, Fn&& fn,
                                    Args&&... args) {
  using alloc_traits = std::allocator_traits<Allocator>;
  using closure_alloc_t = typename alloc_traits::template rebind_alloc<Closure>;
  using closure_alloc_traits = std::allocator_traits<closure_alloc_t>;

  closure_alloc_t closure_alloc(alloc);
  Closure* const closure = closure_alloc_traits::allocate(closure_alloc, 1);
  try {
    closure_alloc_traits::construct(closure_alloc, closure, alloc,
                                    std::forward<Fn>(fn),
                                    std::forward<Args>(args)...);
  } catch (...) {
    closure_alloc_traits::deallocate(closure_alloc, closure, 1);
    throw;
  }
  return closure;
}

/**
 * Owned ALPN selection closure.
 *
 * Out is the public out-parameter proxy (ssl::context::alpn_out). It is a
 * template parameter because the proxy is a member of the public ssl::context
 * type that this detail header cannot depend on. The trampoline builds the
 * proxy and both non-owning views on the spot, so no pointer to the owning
 * bnio context or its slot is ever stored.
 *
 * The trampoline is noexcept and catches everything: no exception may cross
 * the OpenSSL C boundary, and a throwing user callable is reported as
 * SSL_TLSEXT_ERR_ALERT_FATAL, matching the pre-P0 behavior.
 */
template <class Out, class Allocator, class Callback, class... BoundArgs>
class alpn_closure final
    : public callback_closure_base<
          alpn_closure<Out, Allocator, Callback, BoundArgs...>, Allocator> {
  using base_type = callback_closure_base<
      alpn_closure<Out, Allocator, Callback, BoundArgs...>, Allocator>;

 public:
  /**
   * Forms the closure from the allocator copy, the decay-copied callable, and
   * the decay-copied bound arguments.
   */
  alpn_closure(const Allocator& alloc, Callback fn, BoundArgs... args)
      : base_type(alloc), bound_(std::move(fn), std::move(args)...) {}

  /**
   * OpenSSL ALPN selection trampoline.
   *
   * Recovers the closure from the native void* argument with std::launder,
   * builds the out-parameter proxy and the non-owning context and connection
   * views, and invokes the user callable. Exceptions are swallowed and mapped
   * to SSL_TLSEXT_ERR_ALERT_FATAL so no exception escapes into OpenSSL.
   */
  static int trampoline(SSL* ssl, const unsigned char** out,
                        unsigned char* outlen, const unsigned char* in,
                        unsigned int inlen, void* raw) noexcept {
    // OpenSSL holds the arg as void*. The closure object was constructed in
    // storage reached only through that void*, so std::launder is required to
    // recover a pointer to the live alpn_closure object of this exact type
    // before accessing it.
    auto* closure = std::launder(static_cast<alpn_closure*>(raw));
    try {
      Out out_proxy(out, outlen);
      context_view view(SSL_get_SSL_CTX(ssl));
      return std::invoke(closure->bound_, view, stream_view(ssl), out_proxy,
                         std::span<const unsigned char>(in, inlen));
    } catch (...) {
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
  }

 private:
  bound_invoker<Callback, BoundArgs...> bound_;
};

/**
 * Owned SNI servername closure.
 *
 * Out is the public out-parameter proxy (ssl::sni_out), a template parameter
 * for the same reason as alpn_closure: the proxy is declared in the public
 * callback header, and the trampoline builds it on the spot so no pointer to
 * the owning bnio context is ever stored.
 *
 * The trampoline is noexcept and catches everything: no exception may cross
 * the OpenSSL C boundary, and a throwing user callable fails closed by
 * recording SSL_AD_INTERNAL_ERROR in the alert slot (when there is one) and
 * returning SSL_TLSEXT_ERR_ALERT_FATAL. A successful call passes the user's
 * int result through unchanged.
 */
template <class Out, class Allocator, class Callback, class... BoundArgs>
class servername_closure final
    : public callback_closure_base<
          servername_closure<Out, Allocator, Callback, BoundArgs...>,
          Allocator> {
  using base_type = callback_closure_base<
      servername_closure<Out, Allocator, Callback, BoundArgs...>, Allocator>;

 public:
  /**
   * Forms the closure from the allocator copy, the decay-copied callable, and
   * the decay-copied bound arguments.
   */
  servername_closure(const Allocator& alloc, Callback fn, BoundArgs... args)
      : base_type(alloc), bound_(std::move(fn), std::move(args)...) {}

  /**
   * OpenSSL SNI servername trampoline.
   *
   * Recovers the closure from the native void* argument with std::launder,
   * builds the out-parameter proxy and the non-owning context and connection
   * views, and invokes the user callable. Exceptions are swallowed, recorded
   * as SSL_AD_INTERNAL_ERROR when OpenSSL supplied an alert slot, and mapped
   * to SSL_TLSEXT_ERR_ALERT_FATAL so no exception escapes into OpenSSL.
   */
  static int trampoline(SSL* ssl, int* al, void* raw) noexcept {
    // OpenSSL holds the arg as void*. The closure object was constructed in
    // storage reached only through that void*, so std::launder is required to
    // recover a pointer to the live servername_closure object of this exact
    // type before accessing it.
    auto* closure = std::launder(static_cast<servername_closure*>(raw));
    try {
      Out out(ssl, al);
      context_view view(SSL_get_SSL_CTX(ssl));
      return std::invoke(closure->bound_, view, stream_view(ssl), out);
    } catch (...) {
      if (al != nullptr) {
        *al = SSL_AD_INTERNAL_ERROR;
      }
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
  }

 private:
  bound_invoker<Callback, BoundArgs...> bound_;
};

/**
 * Owned client_hello closure.
 *
 * Out is the public out-parameter proxy (ssl::alert_out), a template
 * parameter for the same reason as alpn_closure. The result enum's values
 * match the SSL_CLIENT_HELLO_* constants, so the trampoline maps the user
 * result with static_cast<int> and has no value-mapping branch.
 *
 * The trampoline is noexcept and catches everything: no exception may cross
 * the OpenSSL C boundary, and a throwing user callable fails closed by
 * recording SSL_AD_INTERNAL_ERROR in the alert slot (when there is one) and
 * returning SSL_CLIENT_HELLO_ERROR. A successful call maps the user result
 * directly to the OpenSSL return value.
 */
template <class Out, class Allocator, class Callback, class... BoundArgs>
class client_hello_closure final
    : public callback_closure_base<
          client_hello_closure<Out, Allocator, Callback, BoundArgs...>,
          Allocator> {
  using base_type = callback_closure_base<
      client_hello_closure<Out, Allocator, Callback, BoundArgs...>, Allocator>;

 public:
  /**
   * Forms the closure from the allocator copy, the decay-copied callable, and
   * the decay-copied bound arguments.
   */
  client_hello_closure(const Allocator& alloc, Callback fn, BoundArgs... args)
      : base_type(alloc), bound_(std::move(fn), std::move(args)...) {}

  /**
   * OpenSSL client_hello trampoline.
   *
   * Recovers the closure from the native void* argument with std::launder,
   * builds the out-parameter proxy and the non-owning context and connection
   * views, and invokes the user callable. Exceptions are swallowed, recorded
   * as SSL_AD_INTERNAL_ERROR when OpenSSL supplied an alert slot, and mapped
   * to SSL_CLIENT_HELLO_ERROR so no exception escapes into OpenSSL.
   */
  static int trampoline(SSL* ssl, int* al, void* raw) noexcept {
    // OpenSSL holds the arg as void*. The closure object was constructed in
    // storage reached only through that void*, so std::launder is required to
    // recover a pointer to the live client_hello_closure object of this exact
    // type before accessing it.
    auto* closure = std::launder(static_cast<client_hello_closure*>(raw));
    try {
      Out out(al);
      context_view view(SSL_get_SSL_CTX(ssl));
      return static_cast<int>(
          std::invoke(closure->bound_, view, stream_view(ssl), out));
    } catch (...) {
      if (al != nullptr) {
        *al = SSL_AD_INTERNAL_ERROR;
      }
      return SSL_CLIENT_HELLO_ERROR;
    }
  }

 private:
  bound_invoker<Callback, BoundArgs...> bound_;
};

/**
 * Owned per-connection certificate closure.
 *
 * Out is the public out-parameter proxy (ssl::cert_out), a template parameter
 * for the same reason as alpn_closure: the proxy is declared in the public
 * callback header, and the trampoline builds it on the spot so no pointer to
 * the owning bnio context is ever stored.
 *
 * The result enum values match the values OpenSSL expects from a certificate
 * callback, so the trampoline maps the user result with static_cast<int> and
 * has no value-mapping branch.
 *
 * OpenSSL copies cert_cb and cert_cb_arg into every SSL's per-connection CERT
 * at SSL_new and SSL_set_SSL_CTX time. The argument is therefore the raw
 * closure pointer, exactly like the other channels, and the owning
 * ssl::context must outlive every stream that may invoke the callback. The
 * trampoline neither allocates nor reaches any global or ex_data state.
 *
 * The trampoline is noexcept and catches everything: no exception may cross
 * the OpenSSL C boundary, and a throwing user callable fails closed by
 * returning 0 (abort the handshake).
 */
template <class Out, class Allocator, class Callback, class... BoundArgs>
class cert_closure final
    : public callback_closure_base<
          cert_closure<Out, Allocator, Callback, BoundArgs...>, Allocator> {
  using base_type = callback_closure_base<
      cert_closure<Out, Allocator, Callback, BoundArgs...>, Allocator>;

 public:
  /**
   * Forms the closure from the allocator copy, the decay-copied callable, and
   * the decay-copied bound arguments.
   */
  cert_closure(const Allocator& alloc, Callback fn, BoundArgs... args)
      : base_type(alloc), bound_(std::move(fn), std::move(args)...) {}

  /**
   * OpenSSL certificate trampoline.
   *
   * raw is the raw pointer to this closure, copied by OpenSSL into the
   * connection's per-connection CERT together with the callback. The owning
   * context must keep the closure alive for as long as the connection may
   * invoke the callback. Builds the out-parameter proxy and the non-owning
   * context and connection views, invokes the user callable, and maps the
   * result directly to the OpenSSL return value. Exceptions are swallowed and
   * mapped to 0 (abort the handshake, fail closed) so no exception escapes
   * into OpenSSL.
   */
  static int trampoline(SSL* ssl, void* raw) noexcept {
    // OpenSSL holds the arg as void*. The closure object was constructed in
    // storage reached only through that void*, so std::launder is required to
    // recover a pointer to the live cert_closure object of this exact type
    // before accessing it.
    auto* closure = std::launder(static_cast<cert_closure*>(raw));
    try {
      Out out(ssl);
      context_view view(SSL_get_SSL_CTX(ssl));
      return static_cast<int>(
          std::invoke(closure->bound_, view, stream_view(ssl), out));
    } catch (...) {
      return 0;
    }
  }

 private:
  bound_invoker<Callback, BoundArgs...> bound_;
};

}  // namespace bnio::ssl::detail

#endif  // BNIO_SSL_DETAIL_OWNED_CALLBACK_H_
