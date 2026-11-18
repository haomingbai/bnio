/**
 * @file owned_callback.h
 * @brief Shared helpers for heap-allocated owned callback closures.
 *
 * Each closure is one heap allocation owning a decay-copied callable and its
 * arguments, reached from the native callback through its void* argument and
 * destroyed through callback_base::destroy(); callback_slot keeps the pointer
 * only as an ownership ledger.
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
 * destroy() rebinds the stored allocator, destroys the closure, and
 * deallocates its storage; a closure is never deleted through a base pointer.
 */
template <class Concrete, class Allocator>
class callback_closure_base : public callback_base {
 public:
  /**
   * Retains the allocator used to allocate the closure.
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
 * Owned callable plus its bound arguments, all stored by value after decay.
 *
 * operator() passes the framework arguments first, then every bound argument
 * as an lvalue, like std::bind: plain arguments are persistent mutable state,
 * and a std::reference_wrapper is unwrapped on each call.
 */
template <class Fn, class... Args>
class bound_invoker {
 public:
  /**
   * Moves or copies the callable and bound arguments into the closure.
   */
  template <class F, class... As>
  explicit bound_invoker(F&& fn, As&&... args)
      : fn_(std::forward<F>(fn)), args_(std::forward<As>(args)...) {}

  /**
   * Invokes the callable with the framework arguments first.
   */
  template <class... FrameworkArgs>
  decltype(auto) operator()(FrameworkArgs&&... args) {
    return invoke(std::index_sequence_for<Args...>{},
                  std::forward<FrameworkArgs>(args)...);
  }

 private:
  /**
   * Unwraps a std::reference_wrapper to the referenced lvalue.
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
 * Allocates and constructs one closure with the strong exception guarantee;
 * construction failure deallocates the storage and rethrows.
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
 * Owned ALPN selection closure; Out is the public alpn_out proxy, a template
 * parameter so this header need not depend on ssl::context.
 */
template <class Out, class Allocator, class Callback, class... BoundArgs>
class alpn_closure final
    : public callback_closure_base<
          alpn_closure<Out, Allocator, Callback, BoundArgs...>, Allocator> {
  using base_type = callback_closure_base<
      alpn_closure<Out, Allocator, Callback, BoundArgs...>, Allocator>;

 public:
  /**
   * Forms the closure from the allocator copy, callable, and bound arguments.
   */
  alpn_closure(const Allocator& alloc, Callback fn, BoundArgs... args)
      : base_type(alloc), bound_(std::move(fn), std::move(args)...) {}

  /**
   * OpenSSL ALPN trampoline; exceptions map to SSL_TLSEXT_ERR_ALERT_FATAL.
   */
  static int trampoline(SSL* ssl, const unsigned char** out,
                        unsigned char* outlen, const unsigned char* in,
                        unsigned int inlen, void* raw) noexcept {
    // std::launder recovers the live closure constructed in raw's storage.
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
 * Owned SNI servername closure; Out is the public sni_out proxy, templated for
 * the same reason as alpn_closure.
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
   * Forms the closure from the allocator copy, callable, and bound arguments.
   */
  servername_closure(const Allocator& alloc, Callback fn, BoundArgs... args)
      : base_type(alloc), bound_(std::move(fn), std::move(args)...) {}

  /**
   * OpenSSL SNI trampoline; exceptions record SSL_AD_INTERNAL_ERROR when an
   * alert slot exists and map to SSL_TLSEXT_ERR_ALERT_FATAL.
   */
  static int trampoline(SSL* ssl, int* al, void* raw) noexcept {
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
 * Owned client_hello closure; Out is the public alert_out proxy. The result
 * enum values match SSL_CLIENT_HELLO_*, so results map by static_cast<int>.
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
   * Forms the closure from the allocator copy, callable, and bound arguments.
   */
  client_hello_closure(const Allocator& alloc, Callback fn, BoundArgs... args)
      : base_type(alloc), bound_(std::move(fn), std::move(args)...) {}

  /**
   * OpenSSL client_hello trampoline; exceptions record SSL_AD_INTERNAL_ERROR
   * when an alert slot exists and map to SSL_CLIENT_HELLO_ERROR.
   */
  static int trampoline(SSL* ssl, int* al, void* raw) noexcept {
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
 * Owned certificate closure; Out is the public cert_out proxy. OpenSSL copies
 * cert_cb_arg (this closure) into each SSL at SSL_new and SSL_set_SSL_CTX, so
 * the owning context must outlive every stream that may invoke it.
 */
template <class Out, class Allocator, class Callback, class... BoundArgs>
class cert_closure final
    : public callback_closure_base<
          cert_closure<Out, Allocator, Callback, BoundArgs...>, Allocator> {
  using base_type = callback_closure_base<
      cert_closure<Out, Allocator, Callback, BoundArgs...>, Allocator>;

 public:
  /**
   * Forms the closure from the allocator copy, callable, and bound arguments.
   */
  cert_closure(const Allocator& alloc, Callback fn, BoundArgs... args)
      : base_type(alloc), bound_(std::move(fn), std::move(args)...) {}

  /**
   * OpenSSL certificate trampoline. raw is copied by OpenSSL into the
   * connection's per-connection CERT together with the callback, so the owning
   * context must keep it alive while the connection may invoke the callback;
   * exceptions map to 0 (abort the handshake).
   */
  static int trampoline(SSL* ssl, void* raw) noexcept {
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
