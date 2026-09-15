/**
 * @file context.h
 * @brief RAII SSL_CTX owner.
 */

#pragma once
#ifndef BNIO_SSL_CONTEXT_H_
#define BNIO_SSL_CONTEXT_H_

#include <bnio/export.h>
#include <openssl/ssl.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio {

/**
 * OpenSSL SSL_CTX method selector.
 */
enum class ssl_context_method {
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
enum class ssl_handshake_type {
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
[[nodiscard]] BNIO_EXPORT const std::error_category&
openssl_error_category() noexcept;

/**
 * Creates an error_code in the OpenSSL error category.
 */
[[nodiscard]] BNIO_EXPORT std::error_code make_openssl_error(
    unsigned long code) noexcept;

/**
 * Returns the error_code bnio reports when an SSL operation's failure path
 * recorded no OpenSSL error at all (for example, a handshake on an invalid
 * stream never reaches OpenSSL). The value lives in the OpenSSL error
 * category, never collides with a real OpenSSL error code, and does not
 * represent any TLS-level failure.
 */
[[nodiscard]] BNIO_EXPORT std::error_code make_no_ssl_error() noexcept;

/**
 * RAII owner for an OpenSSL SSL_CTX object.
 *
 * ssl_context owns the native context and frees it on destruction. It is
 * move-only because the native SSL_CTX ownership is unique.
 */
class BNIO_EXPORT ssl_context {
 public:
  /**
   * Creates an SSL context for the selected TLS method.
   */
  explicit ssl_context(
      ssl_context_method method = ssl_context_method::tls) noexcept;

  /**
   * Frees the owned SSL_CTX, if any.
   */
  ~ssl_context() noexcept;

  /**
   * Copy construction is disabled because the context owns an SSL_CTX.
   */
  ssl_context(const ssl_context&) = delete;

  /**
   * Copy assignment is disabled because the context owns an SSL_CTX.
   */
  ssl_context& operator=(const ssl_context&) = delete;

  /**
   * Moves SSL_CTX ownership from another context.
   */
  ssl_context(ssl_context&& other) noexcept;

  /**
   * Frees the current SSL_CTX and moves ownership from another context.
   */
  ssl_context& operator=(ssl_context&& other) noexcept;

  /**
   * Returns the owned native SSL_CTX pointer, or nullptr when invalid.
   */
  [[nodiscard]] SSL_CTX* native_handle() const noexcept { return context_; }

  /**
   * Returns whether this context owns a native SSL_CTX.
   */
  [[nodiscard]] bool valid() const noexcept { return context_ != nullptr; }

  /**
   * Loads a certificate chain file into the context.
   */
  [[nodiscard]] std::error_code use_certificate_chain_file(
      const char* path) noexcept;

  /**
   * Loads a PEM private key file into the context.
   */
  [[nodiscard]] std::error_code use_private_key_file(const char* path) noexcept;

  /**
   * Checks whether the loaded private key matches the certificate.
   */
  [[nodiscard]] std::error_code check_private_key() noexcept;

  /**
   * Sets OpenSSL certificate verification flags for the context.
   */
  void set_verify_mode(int mode) noexcept;

  /**
   * Callback signature matching OpenSSL's SSL_CTX_set_alpn_select_cb
   * protocol. Returns SSL_TLSEXT_ERR_OK, SSL_TLSEXT_ERR_NOACK,
   * SSL_TLSEXT_ERR_ALERT_FATAL, etc.
   */
  using alpn_select_cb = int (*)(SSL* ssl, const unsigned char** out,
                                 unsigned char* outlen, const unsigned char* in,
                                 unsigned int inlen, void* arg);

  /**
   * Installs an ALPN selection callback. The context does not own arg; the
   * caller must keep it alive for the lifetime of the context.
   */
  void set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept;

  /**
   * Write-back proxy handed to an ALPN selection callback installed through
   * set_alpn_callback. Call set() exactly once to select a protocol; leave
   * it untouched to skip ALPN negotiation.
   */
  class alpn_out {
   public:
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
   * previously installed closure.
   *
   * The user callable is invoked as: int fn(ssl_context& ctx, alpn_out& out,
   * std::span<const unsigned char> protocols, BoundArgs... args) where ctx is
   * the context this callback was installed on (do not move the ssl_context
   * after installing: the callback would receive a dangling reference), out
   * writes the selected protocol via set(), and protocols covers OpenSSL's
   * wire-format protocol list. The returned int is passed through to OpenSSL
   * (SSL_TLSEXT_ERR_OK, SSL_TLSEXT_ERR_NOACK, ...). Exceptions escaping fn
   * are swallowed and reported as SSL_TLSEXT_ERR_ALERT_FATAL.
   *
   * std::reference_wrapper arguments are stored as references and forwarded
   * by reference on every invocation (like std::bind); the referenced object
   * must outlive the closure (until reinstall or context destruction).
   *
   * Allocator is a fit-for-purpose allocator type rebound to the closure
   * state; a default-constructed instance is used. To supply one, name it as
   * the first template argument: ctx.set_alpn_callback<MyAlloc>(fn, args...).
   * Allocation and construction may throw (like std::thread construction).
   */
  template <class Allocator = std::allocator<std::byte>, class Fn,
            class... Args>
  void set_alpn_callback(Fn&& f, Args&&... args) {
    using closure_type =
        alpn_closure<Allocator, std::decay_t<Fn>, std::decay_t<Args>...>;
    using alloc_traits = std::allocator_traits<Allocator>;
    using closure_alloc_t =
        typename alloc_traits::template rebind_alloc<closure_type>;
    using closure_alloc_traits = std::allocator_traits<closure_alloc_t>;

    closure_alloc_t closure_alloc(Allocator{});
    closure_type* closure = closure_alloc_traits::allocate(closure_alloc, 1);
    try {
      closure_alloc_traits::construct(closure_alloc, closure, this, Allocator{},
                                      std::forward<Fn>(f),
                                      std::forward<Args>(args)...);
    } catch (...) {
      closure_alloc_traits::deallocate(closure_alloc, closure, 1);
      throw;
    }
    store_owned_alpn_arg(closure);
    set_alpn_select_cb(&closure_type::trampoline, closure);
  }

 private:
  /**
   * Destruction interface for callback state owned by ssl_context. Concrete
   * argument types know their own allocator and implement destroy() to
   * destroy and deallocate themselves; ssl_context only ever calls
   * destroy() and never deletes through this base pointer.
   */
  class alpn_arg_base {
   public:
    /**
     * Destroys and deallocates the concrete argument object.
     */
    virtual void destroy() noexcept = 0;

   protected:
    // Protected non-virtual destructor: destruction always goes through
    // destroy(), never through a base-class delete.
    alpn_arg_base() = default;
    ~alpn_arg_base() = default;
  };

  /**
   * Heap closure state for set_alpn_callback: the owning context, the
   * rebound allocator copy used to allocate this object, and a single bound
   * callable formed from the decay-copied user callable and its decay-copied
   * bound arguments at construction time. Bound arguments live as persistent
   * state inside the closure: every invocation sees the same objects, and
   * user code may mutate them across calls.
   */
  template <class Allocator, class Callback, class... BoundArgs>
  class alpn_closure final : public alpn_arg_base {
   private:
    /**
     * Forms the single bound callable: binds the user callable to the
     * framework parameters (context, out proxy, protocol list) followed by
     * the bound arguments. reference_wrapper captures stay wrapped here and
     * are unwrapped per invocation by unwrap_arg.
     */
    template <class F, class... As>
    static auto make_bound(F&& f, As&&... as) {
      return [f = std::forward<F>(f), ... captured = std::forward<As>(as)](
                 ssl_context& ctx, alpn_out& out,
                 std::span<const unsigned char> protocols) mutable -> int {
        return std::invoke(f, ctx, out, protocols, unwrap_arg(captured)...);
      };
    }

    /**
     * Unwraps a reference_wrapper binding, forwarding the referenced object
     * as an lvalue reference (std::bind semantics).
     */
    template <class T>
    static constexpr T& unwrap_arg(std::reference_wrapper<T> w) noexcept {
      return w.get();
    }

    /**
     * Passes plain bound arguments through as lvalues.
     */
    template <class T>
    static constexpr T& unwrap_arg(T& v) noexcept {
      return v;
    }

   public:
    alpn_closure(ssl_context* owner, const Allocator& alloc, Callback fn,
                 BoundArgs... args)
        : owner_(owner),
          allocator_(alloc),
          bound_(make_bound(std::move(fn), std::move(args)...)) {}

    // this is already the concrete alpn_closure type here; the allocator
    // copy stored at allocation time keeps deallocate symmetric.
    void destroy() noexcept override {
      using closure_alloc_t = typename std::allocator_traits<
          Allocator>::template rebind_alloc<alpn_closure>;
      closure_alloc_t alloc(allocator_);
      std::allocator_traits<closure_alloc_t>::destroy(alloc, this);
      std::allocator_traits<closure_alloc_t>::deallocate(alloc, this, 1);
    }

    static int trampoline(SSL* /*ssl*/, const unsigned char** out,
                          unsigned char* outlen, const unsigned char* in,
                          unsigned int inlen, void* raw) noexcept {
      // OpenSSL holds the arg as void*. The closure object was constructed in
      // storage reached only through that void*, so std::launder is required
      // to recover a pointer to the live alpn_closure object of this exact
      // type before accessing it.
      auto* closure = std::launder(static_cast<alpn_closure*>(raw));
      try {
        alpn_out out_proxy(out, outlen);
        return std::invoke(closure->bound_, *closure->owner_, out_proxy,
                           std::span<const unsigned char>(in, inlen));
      } catch (...) {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
      }
    }

   private:
    ssl_context* owner_;
    Allocator allocator_;
    decltype(make_bound(std::declval<Callback>(),
                        std::declval<BoundArgs>()...)) bound_;
  };

  SSL_CTX* context_ = nullptr;
  alpn_arg_base* owned_alpn_arg_ = nullptr;

  /**
   * Takes ownership of owned_arg, destroying any previously owned argument.
   */
  void store_owned_alpn_arg(alpn_arg_base* owned_arg) noexcept;
};

}  // namespace bnio

#endif  // BNIO_SSL_CONTEXT_H_
