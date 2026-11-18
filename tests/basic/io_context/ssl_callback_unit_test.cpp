/**
 * @file ssl_callback_unit_test.cpp
 * @brief Unit tests for the shared owned-callback infrastructure (P0).
 *
 * These tests drive the detail helpers directly (bound_invoker,
 * callback_closure_base, make_closure, alpn_closure) plus the owning
 * callback_slot and the public ALPN install path. The goal is behavioral
 * coverage of every new branch: bound-argument forwarding and unwrapping,
 * slot move/reset ownership transfer, allocator symmetry, the strong
 * exception guarantee, and the ALPN trampoline's success, pass-through, and
 * exception paths.
 */

#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../support/io_context/ssl_test_support.h"

namespace {

namespace detail = bnio::ssl::detail;

using bnio::ssl::context;
using bnio::ssl::context_view;
using bnio::ssl::stream_view;

// ---------------------------------------------------------------------------
// bound_invoker
// ---------------------------------------------------------------------------

// Records invocation order implicitly through persisted bound state: the
// callable receives the framework argument first, then the stored arguments
// as non-const lvalues, and mutates both.
struct invoker_recorder {
  int calls = 0;

  std::string operator()(int framework, std::string& label, int& counter) {
    ++calls;
    ++counter;
    label += '!';
    return label + std::to_string(framework) + ':' + std::to_string(counter);
  }
};

TEST(SslCallbackUnitTest, bound_invoker_forwards_framework_then_bound_lvalues) {
  detail::bound_invoker<invoker_recorder, std::string, int> invoker(
      invoker_recorder{}, std::string("label"), 0);

  // The second call proves the stored arguments are persistent mutable state:
  // the label kept its appended '!' and the counter kept counting.
  EXPECT_EQ(invoker(7), "label!7:1");
  EXPECT_EQ(invoker(8), "label!!8:2");
}

// Non-copyable and non-movable on purpose: binding it as a reference is the
// only way to store it, so a successful invocation proves reference_wrapper
// was unwrapped rather than copied.
struct noncopyable_counter {
  int value = 0;
  noncopyable_counter() = default;
  noncopyable_counter(const noncopyable_counter&) = delete;
  noncopyable_counter& operator=(const noncopyable_counter&) = delete;
  noncopyable_counter(noncopyable_counter&&) = delete;
  noncopyable_counter& operator=(noncopyable_counter&&) = delete;
};

struct reference_invoker_recorder {
  int operator()(int framework, noncopyable_counter& counter,
                 const std::string& label) const {
    counter.value += framework;
    return static_cast<int>(label.size()) + counter.value;
  }
};

TEST(SslCallbackUnitTest,
     bound_invoker_unwraps_reference_wrapper_on_every_invocation) {
  noncopyable_counter counter;
  detail::bound_invoker<reference_invoker_recorder,
                        std::reference_wrapper<noncopyable_counter>,
                        std::string>
      invoker(reference_invoker_recorder{}, std::ref(counter),
              std::string("abc"));

  EXPECT_EQ(invoker(4), 3 + 4);
  EXPECT_EQ(counter.value, 4);
  EXPECT_EQ(invoker(6), 3 + 10);
  EXPECT_EQ(counter.value, 10);
}

// ---------------------------------------------------------------------------
// callback_slot ownership
// ---------------------------------------------------------------------------

struct destruction_counter {
  int destroyed = 0;
};

class counting_closure final : public detail::callback_base {
 public:
  explicit counting_closure(destruction_counter* counter) noexcept
      : counter_(counter) {}

  void destroy() noexcept override { ++counter_->destroyed; }

 private:
  destruction_counter* counter_;
};

TEST(SslCallbackUnitTest, callback_slot_reset_destroys_every_channel_once) {
  destruction_counter alpn_counter;
  destruction_counter servername_counter;
  destruction_counter client_hello_counter;
  destruction_counter cert_counter;
  destruction_counter keylog_counter;
  destruction_counter verify_counter;
  destruction_counter info_counter;
  destruction_counter msg_counter;

  counting_closure alpn(&alpn_counter);
  counting_closure servername(&servername_counter);
  counting_closure client_hello(&client_hello_counter);
  counting_closure cert(&cert_counter);
  counting_closure keylog(&keylog_counter);
  counting_closure verify(&verify_counter);
  counting_closure info(&info_counter);
  counting_closure msg(&msg_counter);

  {
    detail::callback_slot slot;
    slot.alpn = &alpn;
    slot.servername = &servername;
    slot.client_hello = &client_hello;
    slot.cert = &cert;
    slot.keylog = &keylog;
    slot.verify = &verify;
    slot.info = &info;
    slot.msg = &msg;

    slot.reset();

    EXPECT_EQ(alpn_counter.destroyed, 1);
    EXPECT_EQ(servername_counter.destroyed, 1);
    EXPECT_EQ(client_hello_counter.destroyed, 1);
    EXPECT_EQ(cert_counter.destroyed, 1);
    EXPECT_EQ(keylog_counter.destroyed, 1);
    EXPECT_EQ(verify_counter.destroyed, 1);
    EXPECT_EQ(info_counter.destroyed, 1);
    EXPECT_EQ(msg_counter.destroyed, 1);
    EXPECT_EQ(slot.alpn, nullptr);
    EXPECT_EQ(slot.servername, nullptr);
    EXPECT_EQ(slot.client_hello, nullptr);
    EXPECT_EQ(slot.cert, nullptr);
    EXPECT_EQ(slot.keylog, nullptr);
    EXPECT_EQ(slot.verify, nullptr);
    EXPECT_EQ(slot.info, nullptr);
    EXPECT_EQ(slot.msg, nullptr);

    // A second reset sees only null pointers and destroys nothing again.
    slot.reset();
    EXPECT_EQ(alpn_counter.destroyed, 1);
    EXPECT_EQ(servername_counter.destroyed, 1);
  }

  // Slot destruction after reset must not destroy anything a second time.
  EXPECT_EQ(alpn_counter.destroyed, 1);
  EXPECT_EQ(cert_counter.destroyed, 1);
}

TEST(SslCallbackUnitTest, callback_slot_move_construction_transfers_ownership) {
  destruction_counter alpn_counter;
  destruction_counter servername_counter;
  destruction_counter client_hello_counter;
  destruction_counter cert_counter;
  counting_closure alpn(&alpn_counter);
  counting_closure servername(&servername_counter);
  counting_closure client_hello(&client_hello_counter);
  counting_closure cert(&cert_counter);

  {
    detail::callback_slot source;
    source.alpn = &alpn;
    source.servername = &servername;
    source.client_hello = &client_hello;
    source.cert = &cert;

    detail::callback_slot moved(std::move(source));
    EXPECT_EQ(source.alpn, nullptr);
    EXPECT_EQ(source.servername, nullptr);
    EXPECT_EQ(source.client_hello, nullptr);
    EXPECT_EQ(source.cert, nullptr);
    EXPECT_EQ(moved.alpn, &alpn);
    EXPECT_EQ(moved.servername, &servername);
    EXPECT_EQ(moved.client_hello, &client_hello);
    EXPECT_EQ(moved.cert, &cert);
    EXPECT_EQ(alpn_counter.destroyed, 0);
    EXPECT_EQ(servername_counter.destroyed, 0);
    EXPECT_EQ(client_hello_counter.destroyed, 0);
    EXPECT_EQ(cert_counter.destroyed, 0);
  }

  EXPECT_EQ(alpn_counter.destroyed, 1);
  EXPECT_EQ(servername_counter.destroyed, 1);
  EXPECT_EQ(client_hello_counter.destroyed, 1);
  EXPECT_EQ(cert_counter.destroyed, 1);
}

TEST(SslCallbackUnitTest, callback_slot_move_assignment_replaces_ownership) {
  destruction_counter old_alpn_counter;
  destruction_counter old_servername_counter;
  destruction_counter new_client_hello_counter;
  destruction_counter new_cert_counter;
  counting_closure old_alpn(&old_alpn_counter);
  counting_closure old_servername(&old_servername_counter);
  counting_closure new_client_hello(&new_client_hello_counter);
  counting_closure new_cert(&new_cert_counter);

  detail::callback_slot target;
  target.alpn = &old_alpn;
  target.servername = &old_servername;

  detail::callback_slot source;
  source.client_hello = &new_client_hello;
  source.cert = &new_cert;

  target = std::move(source);

  // The previous closures are destroyed exactly once, the transferred
  // closures are now owned by the target, and the source is emptied.
  EXPECT_EQ(old_alpn_counter.destroyed, 1);
  EXPECT_EQ(old_servername_counter.destroyed, 1);
  EXPECT_EQ(new_client_hello_counter.destroyed, 0);
  EXPECT_EQ(new_cert_counter.destroyed, 0);
  EXPECT_EQ(target.alpn, nullptr);
  EXPECT_EQ(target.servername, nullptr);
  EXPECT_EQ(target.client_hello, &new_client_hello);
  EXPECT_EQ(target.cert, &new_cert);
  EXPECT_EQ(source.alpn, nullptr);
  EXPECT_EQ(source.servername, nullptr);
  EXPECT_EQ(source.client_hello, nullptr);
  EXPECT_EQ(source.cert, nullptr);

  target.reset();
  EXPECT_EQ(new_client_hello_counter.destroyed, 1);
  EXPECT_EQ(new_cert_counter.destroyed, 1);
  source.reset();
  EXPECT_EQ(new_client_hello_counter.destroyed, 1);
  EXPECT_EQ(new_cert_counter.destroyed, 1);
}

// Self move assignment goes through the this == &other early return and must
// neither destroy nor lose the owned closure. The two-reference helper keeps
// the deliberate self assignment out of a direct -Wself-move diagnostic.
void move_assign(detail::callback_slot& target, detail::callback_slot& source) {
  target = std::move(source);
}

TEST(SslCallbackUnitTest, callback_slot_self_move_assignment_is_noop) {
  destruction_counter counter;
  counting_closure closure(&counter);

  detail::callback_slot slot;
  slot.alpn = &closure;

  move_assign(slot, slot);

  EXPECT_EQ(slot.alpn, &closure);
  EXPECT_EQ(counter.destroyed, 0);

  slot.reset();
  EXPECT_EQ(counter.destroyed, 1);
}

// ---------------------------------------------------------------------------
// Allocator symmetry and the strong exception guarantee
// ---------------------------------------------------------------------------

struct allocation_counters {
  int allocations = 0;
  int deallocations = 0;

  [[nodiscard]] int outstanding() const noexcept {
    return allocations - deallocations;
  }
};

allocation_counters& tracked_allocations() noexcept {
  static allocation_counters counters;
  return counters;
}

void reset_allocation_counters() noexcept {
  tracked_allocations() = allocation_counters{};
}

// Minimal allocator that shares one allocation counter across every rebound
// instantiation, so the closure allocation and its deallocation are observed
// through the same counts.
template <class T>
class counting_allocator {
 public:
  using value_type = T;

  counting_allocator() noexcept = default;

  template <class U>
  counting_allocator(const counting_allocator<U>&) noexcept {}

  [[nodiscard]] T* allocate(std::size_t count) {
    ++tracked_allocations().allocations;
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T* pointer, std::size_t count) noexcept {
    ++tracked_allocations().deallocations;
    std::allocator<T>{}.deallocate(pointer, count);
  }

  template <class U>
  [[nodiscard]] bool operator==(const counting_allocator<U>&) const noexcept {
    return true;
  }

  template <class U>
  [[nodiscard]] bool operator!=(const counting_allocator<U>&) const noexcept {
    return false;
  }
};

int accept_alpn_callback(context_view /*ctx*/, stream_view /*ssl*/,
                         context::alpn_out& /*out*/,
                         std::span<const unsigned char> /*protocols*/) {
  return SSL_TLSEXT_ERR_OK;
}

int accept_servername_callback(context_view /*ctx*/, stream_view /*ssl*/,
                               bnio::ssl::sni_out& /*out*/) {
  return SSL_TLSEXT_ERR_OK;
}

bnio::ssl::client_hello_result accept_client_hello_callback(
    context_view /*ctx*/, stream_view /*ssl*/, bnio::ssl::alert_out& /*out*/) {
  return bnio::ssl::client_hello_result::success;
}

bnio::ssl::cert_result accept_cert_callback(context_view /*ctx*/,
                                            stream_view /*ssl*/,
                                            bnio::ssl::cert_out& /*out*/) {
  return bnio::ssl::cert_result::ok;
}

TEST(SslCallbackUnitTest, alpn_install_balances_allocate_and_destroy) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_alpn_callback<counting_allocator<std::byte>>(accept_alpn_callback);

    EXPECT_EQ(tracked_allocations().allocations, 1);
    EXPECT_EQ(tracked_allocations().deallocations, 0);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  // Context teardown destroys and deallocates the owned closure exactly once.
  EXPECT_EQ(tracked_allocations().allocations, 1);
  EXPECT_EQ(tracked_allocations().deallocations, 1);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

TEST(SslCallbackUnitTest, alpn_reinstall_destroys_previous_closure) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_alpn_callback<counting_allocator<std::byte>>(accept_alpn_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);
    ASSERT_EQ(tracked_allocations().deallocations, 0);

    ctx.set_alpn_callback<counting_allocator<std::byte>>(
        [](context_view, stream_view, context::alpn_out&,
           std::span<const unsigned char>) -> int {
          return SSL_TLSEXT_ERR_NOACK;
        });

    EXPECT_EQ(tracked_allocations().allocations, 2);
    EXPECT_EQ(tracked_allocations().deallocations, 1);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  EXPECT_EQ(tracked_allocations().allocations, 2);
  EXPECT_EQ(tracked_allocations().deallocations, 2);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

// Passed as an lvalue so the decay-copy into the closure throws.
struct copy_throwing_callable {
  copy_throwing_callable() = default;
  copy_throwing_callable(const copy_throwing_callable&) {
    throw std::runtime_error("bound callable copy failed");
  }
  copy_throwing_callable(copy_throwing_callable&&) noexcept = default;
  copy_throwing_callable& operator=(const copy_throwing_callable&) = delete;
  copy_throwing_callable& operator=(copy_throwing_callable&&) = delete;
  ~copy_throwing_callable() = default;

  int operator()(context_view, stream_view, context::alpn_out&,
                 std::span<const unsigned char>) const {
    return SSL_TLSEXT_ERR_OK;
  }
};

TEST(SslCallbackUnitTest, alpn_install_failure_keeps_previous_closure_owned) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_alpn_callback<counting_allocator<std::byte>>(accept_alpn_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);
    ASSERT_EQ(tracked_allocations().deallocations, 0);

    copy_throwing_callable throwing;
    EXPECT_THROW(ctx.set_alpn_callback<counting_allocator<std::byte>>(throwing),
                 std::runtime_error);

    // The failed closure's storage was deallocated, while the previously
    // installed closure was left untouched: only one deallocation happened
    // and one closure is still owned by the context.
    EXPECT_EQ(tracked_allocations().allocations, 2);
    EXPECT_EQ(tracked_allocations().deallocations, 1);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  // The surviving previous closure is destroyed exactly once at teardown, so
  // the total remains balanced and no closure was leaked or double freed.
  EXPECT_EQ(tracked_allocations().allocations, 2);
  EXPECT_EQ(tracked_allocations().deallocations, 2);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

// Fails before handing out storage, so the strong exception guarantee must
// leave the previously installed closure untouched and deallocate nothing.
template <class T>
class failing_allocator {
 public:
  using value_type = T;

  failing_allocator() noexcept = default;

  template <class U>
  failing_allocator(const failing_allocator<U>&) noexcept {}

  [[nodiscard]] T* allocate(std::size_t /*count*/) { throw std::bad_alloc(); }

  void deallocate(T* /*pointer*/, std::size_t /*count*/) noexcept {
    ++tracked_allocations().deallocations;
  }

  template <class U>
  [[nodiscard]] bool operator==(const failing_allocator<U>&) const noexcept {
    return true;
  }

  template <class U>
  [[nodiscard]] bool operator!=(const failing_allocator<U>&) const noexcept {
    return false;
  }
};

TEST(SslCallbackUnitTest,
     alpn_install_allocation_failure_keeps_previous_closure) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_alpn_callback<counting_allocator<std::byte>>(accept_alpn_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);
    ASSERT_EQ(tracked_allocations().deallocations, 0);

    EXPECT_THROW(ctx.set_alpn_callback<failing_allocator<std::byte>>(
                     accept_alpn_callback),
                 std::bad_alloc);

    // The failed attempt allocated nothing and therefore deallocated nothing;
    // the previously installed closure is still owned by the context.
    EXPECT_EQ(tracked_allocations().allocations, 1);
    EXPECT_EQ(tracked_allocations().deallocations, 0);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  EXPECT_EQ(tracked_allocations().allocations, 1);
  EXPECT_EQ(tracked_allocations().deallocations, 1);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

TEST(SslCallbackUnitTest, context_move_transfers_owned_alpn_closure) {
  reset_allocation_counters();
  {
    context original(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(original.valid());
    original.set_alpn_callback<counting_allocator<std::byte>>(
        accept_alpn_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);
    ASSERT_EQ(tracked_allocations().deallocations, 0);

    context moved(std::move(original));
    EXPECT_FALSE(original.valid());
    ASSERT_TRUE(moved.valid());

    context assigned(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(assigned.valid());
    assigned = std::move(moved);
    EXPECT_FALSE(moved.valid());
    ASSERT_TRUE(assigned.valid());

    // Moving transfers the single closure pointer; nothing is destroyed yet.
    EXPECT_EQ(tracked_allocations().allocations, 1);
    EXPECT_EQ(tracked_allocations().deallocations, 0);
  }

  EXPECT_EQ(tracked_allocations().allocations, 1);
  EXPECT_EQ(tracked_allocations().deallocations, 1);
}

TEST(SslCallbackUnitTest, servername_install_and_replace_balance_allocations) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_servername_callback<counting_allocator<std::byte>>(
        accept_servername_callback);

    // The first install owns one closure and has no previous generation.
    EXPECT_EQ(tracked_allocations().allocations, 1);
    EXPECT_EQ(tracked_allocations().deallocations, 0);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);

    // Replacing destroys the previous closure only after the native
    // registration was updated, leaving exactly one owned closure.
    ctx.set_servername_callback<counting_allocator<std::byte>>(
        accept_servername_callback);
    EXPECT_EQ(tracked_allocations().allocations, 2);
    EXPECT_EQ(tracked_allocations().deallocations, 1);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  // Context teardown destroys and deallocates the surviving closure once.
  EXPECT_EQ(tracked_allocations().allocations, 2);
  EXPECT_EQ(tracked_allocations().deallocations, 2);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

TEST(SslCallbackUnitTest,
     client_hello_install_and_replace_balance_allocations) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_client_hello_callback<counting_allocator<std::byte>>(
        accept_client_hello_callback);

    EXPECT_EQ(tracked_allocations().allocations, 1);
    EXPECT_EQ(tracked_allocations().deallocations, 0);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);

    ctx.set_client_hello_callback<counting_allocator<std::byte>>(
        accept_client_hello_callback);
    EXPECT_EQ(tracked_allocations().allocations, 2);
    EXPECT_EQ(tracked_allocations().deallocations, 1);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  EXPECT_EQ(tracked_allocations().allocations, 2);
  EXPECT_EQ(tracked_allocations().deallocations, 2);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

// Passed as an lvalue so the decay-copy into the closure throws.
struct copy_throwing_servername_callable {
  copy_throwing_servername_callable() = default;
  copy_throwing_servername_callable(const copy_throwing_servername_callable&) {
    throw std::runtime_error("bound servername callable copy failed");
  }
  copy_throwing_servername_callable(
      copy_throwing_servername_callable&&) noexcept = default;
  copy_throwing_servername_callable& operator=(
      const copy_throwing_servername_callable&) = delete;
  copy_throwing_servername_callable& operator=(
      copy_throwing_servername_callable&&) = delete;
  ~copy_throwing_servername_callable() = default;

  int operator()(context_view, stream_view, bnio::ssl::sni_out&) {
    return SSL_TLSEXT_ERR_OK;
  }
};

TEST(SslCallbackUnitTest,
     servername_install_failure_keeps_previous_closure_owned) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_servername_callback<counting_allocator<std::byte>>(
        accept_servername_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);

    copy_throwing_servername_callable throwing;
    EXPECT_THROW(
        ctx.set_servername_callback<counting_allocator<std::byte>>(throwing),
        std::runtime_error);

    // The failed closure's storage was deallocated; the previously installed
    // closure is still the sole owned closure.
    EXPECT_EQ(tracked_allocations().allocations, 2);
    EXPECT_EQ(tracked_allocations().deallocations, 1);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  EXPECT_EQ(tracked_allocations().allocations, 2);
  EXPECT_EQ(tracked_allocations().deallocations, 2);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

TEST(SslCallbackUnitTest,
     servername_install_allocation_failure_keeps_previous_closure) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_servername_callback<counting_allocator<std::byte>>(
        accept_servername_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);

    EXPECT_THROW(ctx.set_servername_callback<failing_allocator<std::byte>>(
                     accept_servername_callback),
                 std::bad_alloc);

    EXPECT_EQ(tracked_allocations().allocations, 1);
    EXPECT_EQ(tracked_allocations().deallocations, 0);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  EXPECT_EQ(tracked_allocations().allocations, 1);
  EXPECT_EQ(tracked_allocations().deallocations, 1);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

// Passed as an lvalue so the decay-copy into the closure throws.
struct copy_throwing_client_hello_callable {
  copy_throwing_client_hello_callable() = default;
  copy_throwing_client_hello_callable(
      const copy_throwing_client_hello_callable&) {
    throw std::runtime_error("bound client_hello callable copy failed");
  }
  copy_throwing_client_hello_callable(
      copy_throwing_client_hello_callable&&) noexcept = default;
  copy_throwing_client_hello_callable& operator=(
      const copy_throwing_client_hello_callable&) = delete;
  copy_throwing_client_hello_callable& operator=(
      copy_throwing_client_hello_callable&&) = delete;
  ~copy_throwing_client_hello_callable() = default;

  bnio::ssl::client_hello_result operator()(context_view, stream_view,
                                            bnio::ssl::alert_out&) {
    return bnio::ssl::client_hello_result::success;
  }
};

TEST(SslCallbackUnitTest,
     client_hello_install_failure_keeps_previous_closure_owned) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_client_hello_callback<counting_allocator<std::byte>>(
        accept_client_hello_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);

    copy_throwing_client_hello_callable throwing;
    EXPECT_THROW(
        ctx.set_client_hello_callback<counting_allocator<std::byte>>(throwing),
        std::runtime_error);

    EXPECT_EQ(tracked_allocations().allocations, 2);
    EXPECT_EQ(tracked_allocations().deallocations, 1);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  EXPECT_EQ(tracked_allocations().allocations, 2);
  EXPECT_EQ(tracked_allocations().deallocations, 2);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

TEST(SslCallbackUnitTest,
     client_hello_install_allocation_failure_keeps_previous_closure) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_client_hello_callback<counting_allocator<std::byte>>(
        accept_client_hello_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);

    EXPECT_THROW(ctx.set_client_hello_callback<failing_allocator<std::byte>>(
                     accept_client_hello_callback),
                 std::bad_alloc);

    EXPECT_EQ(tracked_allocations().allocations, 1);
    EXPECT_EQ(tracked_allocations().deallocations, 0);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  EXPECT_EQ(tracked_allocations().allocations, 1);
  EXPECT_EQ(tracked_allocations().deallocations, 1);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

TEST(SslCallbackUnitTest, cert_install_and_replace_balance_allocations) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_cert_callback<counting_allocator<std::byte>>(accept_cert_callback);

    // The first install owns one closure and has no previous generation.
    EXPECT_EQ(tracked_allocations().allocations, 1);
    EXPECT_EQ(tracked_allocations().deallocations, 0);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);

    // Replacing destroys the previous closure only after the native
    // registration was updated, leaving exactly one owned closure.
    ctx.set_cert_callback<counting_allocator<std::byte>>(accept_cert_callback);
    EXPECT_EQ(tracked_allocations().allocations, 2);
    EXPECT_EQ(tracked_allocations().deallocations, 1);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  // Context teardown destroys and deallocates the surviving closure once.
  EXPECT_EQ(tracked_allocations().allocations, 2);
  EXPECT_EQ(tracked_allocations().deallocations, 2);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

// Passed as an lvalue so the decay-copy into the closure throws.
struct copy_throwing_cert_callable {
  copy_throwing_cert_callable() = default;
  copy_throwing_cert_callable(const copy_throwing_cert_callable&) {
    throw std::runtime_error("bound cert callable copy failed");
  }
  copy_throwing_cert_callable(copy_throwing_cert_callable&&) noexcept = default;
  copy_throwing_cert_callable& operator=(const copy_throwing_cert_callable&) =
      delete;
  copy_throwing_cert_callable& operator=(copy_throwing_cert_callable&&) =
      delete;
  ~copy_throwing_cert_callable() = default;

  bnio::ssl::cert_result operator()(context_view, stream_view,
                                    bnio::ssl::cert_out&) {
    return bnio::ssl::cert_result::ok;
  }
};

TEST(SslCallbackUnitTest, cert_install_failure_keeps_previous_closure_owned) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_cert_callback<counting_allocator<std::byte>>(accept_cert_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);

    copy_throwing_cert_callable throwing;
    EXPECT_THROW(ctx.set_cert_callback<counting_allocator<std::byte>>(throwing),
                 std::runtime_error);

    // The failed closure's storage was deallocated; the previously installed
    // closure is still the sole owned closure.
    EXPECT_EQ(tracked_allocations().allocations, 2);
    EXPECT_EQ(tracked_allocations().deallocations, 1);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  EXPECT_EQ(tracked_allocations().allocations, 2);
  EXPECT_EQ(tracked_allocations().deallocations, 2);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

TEST(SslCallbackUnitTest,
     cert_install_allocation_failure_keeps_previous_closure) {
  reset_allocation_counters();
  {
    context ctx(bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ctx.valid());
    ctx.set_cert_callback<counting_allocator<std::byte>>(accept_cert_callback);
    ASSERT_EQ(tracked_allocations().allocations, 1);

    EXPECT_THROW(ctx.set_cert_callback<failing_allocator<std::byte>>(
                     accept_cert_callback),
                 std::bad_alloc);

    EXPECT_EQ(tracked_allocations().allocations, 1);
    EXPECT_EQ(tracked_allocations().deallocations, 0);
    EXPECT_EQ(tracked_allocations().outstanding(), 1);
  }

  EXPECT_EQ(tracked_allocations().allocations, 1);
  EXPECT_EQ(tracked_allocations().deallocations, 1);
  EXPECT_EQ(tracked_allocations().outstanding(), 0);
}

// ---------------------------------------------------------------------------
// alpn_closure trampoline
// ---------------------------------------------------------------------------

struct alpn_trampoline_probe {
  bool invoked = false;
  int result = SSL_TLSEXT_ERR_OK;
  context_view seen_context;
  SSL* seen_ssl = nullptr;
  std::vector<unsigned char> seen_protocols;
  std::vector<std::string> seen_labels;
};

constexpr unsigned char k_selected_protocol[] = {0x02, 'h', '2'};

// Bound callable: the plain std::string is persistent mutable closure state
// (appended to on every call), while the probe is bound as a reference and is
// therefore unwrapped on each invocation.
struct alpn_trampoline_callable {
  int operator()(context_view ctx, stream_view ssl, context::alpn_out& out,
                 std::span<const unsigned char> protocols, std::string& label,
                 alpn_trampoline_probe& probe) const {
    probe.invoked = true;
    probe.seen_context = ctx;
    probe.seen_ssl = ssl.native_handle();
    probe.seen_protocols.assign(protocols.begin(), protocols.end());
    probe.seen_labels.push_back(label);
    label += '!';
    if (probe.result == SSL_TLSEXT_ERR_OK) {
      out.set(k_selected_protocol,
              static_cast<unsigned char>(sizeof(k_selected_protocol)));
    }
    return probe.result;
  }
};

using alpn_trampoline_closure =
    detail::alpn_closure<context::alpn_out, std::allocator<std::byte>,
                         alpn_trampoline_callable, std::string,
                         std::reference_wrapper<alpn_trampoline_probe>>;

template <class Closure>
struct closure_deleter {
  void operator()(Closure* closure) const noexcept { closure->destroy(); }
};

TEST(SslCallbackUnitTest,
     alpn_trampoline_observes_framework_state_and_bound_args) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  alpn_trampoline_probe probe;
  std::unique_ptr<alpn_trampoline_closure,
                  closure_deleter<alpn_trampoline_closure>>
      closure(detail::make_closure<alpn_trampoline_closure>(
          std::allocator<std::byte>{}, alpn_trampoline_callable{},
          std::string("bound"), std::ref(probe)));

  const unsigned char wire[] = {0x02, 'h', '2', 0x08, 'h', 't',
                                't',  'p', '/', '1',  '.', '1'};
  const unsigned char* selected = nullptr;
  unsigned char selected_len = 0;

  // Success path: the user result is passed through and the proxy writes the
  // selection through OpenSSL's out parameters.
  const int result = alpn_trampoline_closure::trampoline(
      ssl.get(), &selected, &selected_len, wire,
      static_cast<unsigned int>(sizeof(wire)), closure.get());
  EXPECT_EQ(result, SSL_TLSEXT_ERR_OK);
  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.seen_context.native_handle(), ctx.native_handle());
  EXPECT_EQ(probe.seen_ssl, ssl.get());
  EXPECT_EQ(probe.seen_protocols,
            std::vector<unsigned char>(std::begin(wire), std::end(wire)));
  EXPECT_EQ(probe.seen_labels, std::vector<std::string>({"bound"}));
  EXPECT_EQ(selected, k_selected_protocol);
  EXPECT_EQ(selected_len,
            static_cast<unsigned char>(sizeof(k_selected_protocol)));

  // Pass-through of the other ALPN results. The proxy is not written on
  // NOACK, and the second call proves the stored string remained mutable
  // closure state ("bound" was appended to during the first call).
  probe.result = SSL_TLSEXT_ERR_NOACK;
  selected = nullptr;
  selected_len = 0;
  EXPECT_EQ(alpn_trampoline_closure::trampoline(
                ssl.get(), &selected, &selected_len, wire,
                static_cast<unsigned int>(sizeof(wire)), closure.get()),
            SSL_TLSEXT_ERR_NOACK);
  EXPECT_EQ(probe.seen_labels, std::vector<std::string>({"bound", "bound!"}));
  EXPECT_EQ(selected, nullptr);
  EXPECT_EQ(selected_len, 0);

  probe.result = SSL_TLSEXT_ERR_ALERT_FATAL;
  EXPECT_EQ(alpn_trampoline_closure::trampoline(
                ssl.get(), &selected, &selected_len, wire,
                static_cast<unsigned int>(sizeof(wire)), closure.get()),
            SSL_TLSEXT_ERR_ALERT_FATAL);
}

TEST(SslCallbackUnitTest, alpn_trampoline_maps_exceptions_to_alert_fatal) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  auto thrower = [](context_view, stream_view, context::alpn_out&,
                    std::span<const unsigned char>) -> int {
    throw std::runtime_error("alpn callback threw");
  };
  using throwing_closure =
      detail::alpn_closure<context::alpn_out, std::allocator<std::byte>,
                           decltype(thrower)>;
  std::unique_ptr<throwing_closure, closure_deleter<throwing_closure>> closure(
      detail::make_closure<throwing_closure>(std::allocator<std::byte>{},
                                             thrower));

  const unsigned char wire[] = {0x02, 'h', '2'};
  const unsigned char* selected = nullptr;
  unsigned char selected_len = 0;

  // No exception may cross the OpenSSL boundary; the throw fails closed with
  // the historical ALPN result.
  EXPECT_EQ(throwing_closure::trampoline(
                ssl.get(), &selected, &selected_len, wire,
                static_cast<unsigned int>(sizeof(wire)), closure.get()),
            SSL_TLSEXT_ERR_ALERT_FATAL);
  EXPECT_EQ(selected, nullptr);
  EXPECT_EQ(selected_len, 0);
}

// ---------------------------------------------------------------------------
// P1 proxies: alert_out and sni_out
// ---------------------------------------------------------------------------

TEST(SslCallbackUnitTest, alert_out_writes_through_and_tolerates_null) {
  int alert = -1;
  bnio::ssl::alert_out slot(&alert);
  slot.set(SSL_AD_ACCESS_DENIED);
  EXPECT_EQ(alert, SSL_AD_ACCESS_DENIED);

  // A proxy over a null alert slot is reachable through the public
  // constructor; the write must be a no-op rather than a null dereference.
  bnio::ssl::alert_out no_slot(nullptr);
  no_slot.set(SSL_AD_INTERNAL_ERROR);
}

TEST(SslCallbackUnitTest, sni_out_set_alert_writes_through_and_tolerates_null) {
  int alert = -1;
  bnio::ssl::sni_out with_slot(nullptr, &alert);
  with_slot.set_alert(SSL_AD_BAD_CERTIFICATE);
  EXPECT_EQ(alert, SSL_AD_BAD_CERTIFICATE);

  bnio::ssl::sni_out no_slot(nullptr, nullptr);
  no_slot.set_alert(SSL_AD_INTERNAL_ERROR);
}

TEST(SslCallbackUnitTest,
     sni_out_set_context_rejects_null_ssl_and_invalid_context) {
  context source(bnio::ssl::context_method::tls_server);
  context target(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(source.valid());
  ASSERT_TRUE(target.valid());

  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(source.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  const context_view invalid;

  // A null connection is reported before the context is inspected.
  EXPECT_EQ(bnio::ssl::sni_out(nullptr, nullptr).set_context(target.view()),
            bnio::ssl::make_no_ssl_error());
  EXPECT_EQ(bnio::ssl::sni_out(nullptr, nullptr).set_context(invalid),
            bnio::ssl::make_no_ssl_error());

  // A valid connection with an invalid context is rejected as well.
  EXPECT_EQ(bnio::ssl::sni_out(ssl.get(), nullptr).set_context(invalid),
            bnio::ssl::make_no_ssl_error());
  EXPECT_EQ(SSL_get_SSL_CTX(ssl.get()), source.native_handle());

  // The switch mirrors SSL_set_SSL_CTX: no error, and the connection now
  // points at the new native context.
  EXPECT_FALSE(
      bnio::ssl::sni_out(ssl.get(), nullptr).set_context(target.view()));
  EXPECT_EQ(SSL_get_SSL_CTX(ssl.get()), target.native_handle());
}

// ---------------------------------------------------------------------------
// servername_closure trampoline
// ---------------------------------------------------------------------------

struct servername_trampoline_probe {
  bool invoked = false;
  int calls = 0;
  int result = SSL_TLSEXT_ERR_OK;
  int alert_to_set = -1;
  bnio::ssl::context* replacement = nullptr;
  std::error_code switch_error;
  context_view seen_context;
  SSL* seen_ssl = nullptr;
};

// Bound callable for the SNI trampoline: the probe is bound by reference
// (std::ref) and carries both the configured result and the observations.
struct servername_trampoline_callable {
  int operator()(context_view ctx, stream_view ssl, bnio::ssl::sni_out& out,
                 servername_trampoline_probe& probe) {
    probe.invoked = true;
    ++probe.calls;
    probe.seen_context = ctx;
    probe.seen_ssl = ssl.native_handle();
    if (probe.alert_to_set >= 0) {
      out.set_alert(probe.alert_to_set);
    }
    if (probe.replacement != nullptr) {
      probe.switch_error = out.set_context(probe.replacement->view());
    }
    return probe.result;
  }
};

using servername_trampoline_closure = detail::servername_closure<
    bnio::ssl::sni_out, std::allocator<std::byte>,
    servername_trampoline_callable,
    std::reference_wrapper<servername_trampoline_probe>>;

TEST(SslCallbackUnitTest, servername_trampoline_passes_every_result_through) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  servername_trampoline_probe probe;
  std::unique_ptr<servername_trampoline_closure,
                  closure_deleter<servername_trampoline_closure>>
      closure(detail::make_closure<servername_trampoline_closure>(
          std::allocator<std::byte>{}, servername_trampoline_callable{},
          std::ref(probe)));

  const int results[] = {SSL_TLSEXT_ERR_OK, SSL_TLSEXT_ERR_ALERT_WARNING,
                         SSL_TLSEXT_ERR_ALERT_FATAL, SSL_TLSEXT_ERR_NOACK};
  for (const int expected : results) {
    probe.result = expected;
    int alert = -1;
    EXPECT_EQ(closure->trampoline(ssl.get(), &alert, closure.get()), expected);
    // The callable recorded no alert, so the alert slot stays untouched.
    EXPECT_EQ(alert, -1);
  }
  EXPECT_EQ(probe.calls, 4);
  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.seen_context.native_handle(), ctx.native_handle());
  EXPECT_EQ(probe.seen_ssl, ssl.get());

  // A fatal result with a recorded alert writes the alert through the proxy.
  probe.result = SSL_TLSEXT_ERR_ALERT_FATAL;
  probe.alert_to_set = SSL_AD_ACCESS_DENIED;
  int alert = -1;
  EXPECT_EQ(closure->trampoline(ssl.get(), &alert, closure.get()),
            SSL_TLSEXT_ERR_ALERT_FATAL);
  EXPECT_EQ(alert, SSL_AD_ACCESS_DENIED);

  // OpenSSL may supply no alert slot; the proxy write must be a no-op.
  EXPECT_EQ(closure->trampoline(ssl.get(), nullptr, closure.get()),
            SSL_TLSEXT_ERR_ALERT_FATAL);
}

TEST(SslCallbackUnitTest,
     servername_trampoline_switches_context_through_proxy) {
  context source(bnio::ssl::context_method::tls_server);
  context target(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(source.valid());
  ASSERT_TRUE(target.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(source.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  servername_trampoline_probe probe;
  probe.replacement = &target;
  std::unique_ptr<servername_trampoline_closure,
                  closure_deleter<servername_trampoline_closure>>
      closure(detail::make_closure<servername_trampoline_closure>(
          std::allocator<std::byte>{}, servername_trampoline_callable{},
          std::ref(probe)));

  int alert = -1;
  EXPECT_EQ(closure->trampoline(ssl.get(), &alert, closure.get()),
            SSL_TLSEXT_ERR_OK);
  EXPECT_FALSE(probe.switch_error);
  // The callback observed the original context; the switch took effect on
  // the connection afterwards.
  EXPECT_EQ(probe.seen_context.native_handle(), source.native_handle());
  EXPECT_EQ(SSL_get_SSL_CTX(ssl.get()), target.native_handle());
}

struct throwing_servername_callable {
  int operator()(context_view, stream_view, bnio::ssl::sni_out&) const {
    throw std::runtime_error("servername callback threw");
  }
};

using throwing_servername_closure =
    detail::servername_closure<bnio::ssl::sni_out, std::allocator<std::byte>,
                               throwing_servername_callable>;

TEST(SslCallbackUnitTest,
     servername_trampoline_maps_exceptions_to_internal_error) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  std::unique_ptr<throwing_servername_closure,
                  closure_deleter<throwing_servername_closure>>
      closure(detail::make_closure<throwing_servername_closure>(
          std::allocator<std::byte>{}, throwing_servername_callable{}));

  // No exception may cross the OpenSSL boundary: the fallback is a fatal
  // alert carrying SSL_AD_INTERNAL_ERROR.
  int alert = -1;
  EXPECT_EQ(closure->trampoline(ssl.get(), &alert, closure.get()),
            SSL_TLSEXT_ERR_ALERT_FATAL);
  EXPECT_EQ(alert, SSL_AD_INTERNAL_ERROR);

  // With no alert slot the same result is returned without writing.
  EXPECT_EQ(closure->trampoline(ssl.get(), nullptr, closure.get()),
            SSL_TLSEXT_ERR_ALERT_FATAL);
}

// ---------------------------------------------------------------------------
// client_hello_closure trampoline
// ---------------------------------------------------------------------------

static_assert(static_cast<int>(bnio::ssl::client_hello_result::error) ==
              SSL_CLIENT_HELLO_ERROR);
static_assert(static_cast<int>(bnio::ssl::client_hello_result::success) ==
              SSL_CLIENT_HELLO_SUCCESS);
static_assert(static_cast<int>(bnio::ssl::client_hello_result::retry) ==
              SSL_CLIENT_HELLO_RETRY);

struct client_hello_trampoline_probe {
  bool invoked = false;
  int calls = 0;
  bnio::ssl::client_hello_result result =
      bnio::ssl::client_hello_result::success;
  int alert_to_set = -1;
  context_view seen_context;
  SSL* seen_ssl = nullptr;
};

// Bound callable for the client_hello trampoline: the probe is bound by
// reference and carries the configured result and the observations.
struct client_hello_trampoline_callable {
  bnio::ssl::client_hello_result operator()(
      context_view ctx, stream_view ssl, bnio::ssl::alert_out& out,
      client_hello_trampoline_probe& probe) {
    probe.invoked = true;
    ++probe.calls;
    probe.seen_context = ctx;
    probe.seen_ssl = ssl.native_handle();
    if (probe.alert_to_set >= 0) {
      out.set(probe.alert_to_set);
    }
    return probe.result;
  }
};

using client_hello_trampoline_closure = detail::client_hello_closure<
    bnio::ssl::alert_out, std::allocator<std::byte>,
    client_hello_trampoline_callable,
    std::reference_wrapper<client_hello_trampoline_probe>>;

TEST(SslCallbackUnitTest,
     client_hello_trampoline_maps_every_result_to_the_wire_value) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  client_hello_trampoline_probe probe;
  std::unique_ptr<client_hello_trampoline_closure,
                  closure_deleter<client_hello_trampoline_closure>>
      closure(detail::make_closure<client_hello_trampoline_closure>(
          std::allocator<std::byte>{}, client_hello_trampoline_callable{},
          std::ref(probe)));

  struct mapping {
    bnio::ssl::client_hello_result result;
    int wire;
  };
  const mapping mappings[] = {
      {bnio::ssl::client_hello_result::success, SSL_CLIENT_HELLO_SUCCESS},
      {bnio::ssl::client_hello_result::error, SSL_CLIENT_HELLO_ERROR},
      {bnio::ssl::client_hello_result::retry, SSL_CLIENT_HELLO_RETRY},
  };
  for (const mapping& entry : mappings) {
    probe.result = entry.result;
    int alert = -1;
    EXPECT_EQ(closure->trampoline(ssl.get(), &alert, closure.get()),
              entry.wire);
    EXPECT_EQ(alert, -1);
  }
  EXPECT_EQ(probe.calls, 3);
  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.seen_context.native_handle(), ctx.native_handle());
  EXPECT_EQ(probe.seen_ssl, ssl.get());

  // The alert_out proxy writes the configured alert for an error result.
  probe.result = bnio::ssl::client_hello_result::error;
  probe.alert_to_set = SSL_AD_ACCESS_DENIED;
  int alert = -1;
  EXPECT_EQ(closure->trampoline(ssl.get(), &alert, closure.get()),
            SSL_CLIENT_HELLO_ERROR);
  EXPECT_EQ(alert, SSL_AD_ACCESS_DENIED);

  // A null alert slot makes the proxy write a no-op.
  EXPECT_EQ(closure->trampoline(ssl.get(), nullptr, closure.get()),
            SSL_CLIENT_HELLO_ERROR);
}

struct throwing_client_hello_callable {
  bnio::ssl::client_hello_result operator()(context_view, stream_view,
                                            bnio::ssl::alert_out&) const {
    throw std::runtime_error("client_hello callback threw");
  }
};

using throwing_client_hello_closure =
    detail::client_hello_closure<bnio::ssl::alert_out,
                                 std::allocator<std::byte>,
                                 throwing_client_hello_callable>;

TEST(SslCallbackUnitTest,
     client_hello_trampoline_maps_exceptions_to_internal_error) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  std::unique_ptr<throwing_client_hello_closure,
                  closure_deleter<throwing_client_hello_closure>>
      closure(detail::make_closure<throwing_client_hello_closure>(
          std::allocator<std::byte>{}, throwing_client_hello_callable{}));

  // No exception may cross the OpenSSL boundary: the fallback is
  // SSL_CLIENT_HELLO_ERROR with SSL_AD_INTERNAL_ERROR recorded.
  int alert = -1;
  EXPECT_EQ(closure->trampoline(ssl.get(), &alert, closure.get()),
            SSL_CLIENT_HELLO_ERROR);
  EXPECT_EQ(alert, SSL_AD_INTERNAL_ERROR);

  // With no alert slot the same result is returned without writing.
  EXPECT_EQ(closure->trampoline(ssl.get(), nullptr, closure.get()),
            SSL_CLIENT_HELLO_ERROR);
}

// ---------------------------------------------------------------------------
// cert_out proxy and in-memory installers
// ---------------------------------------------------------------------------

// Owns one parsed copy of the embedded PEM fixture.
struct test_identity {
  std::unique_ptr<X509, decltype(&X509_free)> certificate{nullptr, &X509_free};
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> private_key{
      nullptr, &EVP_PKEY_free};
};

test_identity parse_test_identity() {
  test_identity identity;
  BIO* certificate_bio = BIO_new_mem_buf(
      k_test_certificate.data(), static_cast<int>(k_test_certificate.size()));
  test_require(certificate_bio != nullptr);
  identity.certificate.reset(
      PEM_read_bio_X509(certificate_bio, nullptr, nullptr, nullptr));
  BIO_free(certificate_bio);
  test_require(identity.certificate != nullptr);

  BIO* key_bio = BIO_new_mem_buf(k_test_private_key.data(),
                                 static_cast<int>(k_test_private_key.size()));
  test_require(key_bio != nullptr);
  identity.private_key.reset(
      PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr));
  BIO_free(key_bio);
  test_require(identity.private_key != nullptr);
  return identity;
}

TEST(SslCallbackUnitTest, cert_out_rejects_null_connection) {
  // A cert_out over no connection is reachable through the public
  // constructor; both installers report the dedicated no-OpenSSL-error value
  // without reaching OpenSSL.
  EXPECT_EQ(bnio::ssl::cert_out(nullptr).use_certificate(nullptr),
            bnio::ssl::make_no_ssl_error());
  EXPECT_EQ(bnio::ssl::cert_out(nullptr).use_private_key(nullptr),
            bnio::ssl::make_no_ssl_error());
}

TEST(SslCallbackUnitTest, cert_out_reports_openssl_failures) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  bnio::ssl::cert_out out(ssl.get());

  // A null certificate/key makes the underlying OpenSSL call fail; the proxy
  // reports the recorded OpenSSL error instead of a silent no-op. The value
  // must be a genuine error code, not the "no OpenSSL error was recorded"
  // sentinel the null-connection path reports.
  const std::error_code certificate_error = out.use_certificate(nullptr);
  EXPECT_TRUE(certificate_error);
  EXPECT_EQ(certificate_error.category(), bnio::ssl::openssl_error_category());
  EXPECT_NE(certificate_error, bnio::ssl::make_no_ssl_error());
  EXPECT_FALSE(certificate_error.message().empty());
  const std::error_code private_key_error = out.use_private_key(nullptr);
  EXPECT_TRUE(private_key_error);
  EXPECT_EQ(private_key_error.category(), bnio::ssl::openssl_error_category());
  EXPECT_NE(private_key_error, bnio::ssl::make_no_ssl_error());
  EXPECT_FALSE(private_key_error.message().empty());
}

TEST(SslCallbackUnitTest, cert_out_installs_identity_on_the_connection) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  test_identity identity = parse_test_identity();
  X509* const certificate = identity.certificate.get();
  bnio::ssl::cert_out out(ssl.get());
  EXPECT_FALSE(out.use_certificate(certificate));
  EXPECT_FALSE(out.use_private_key(identity.private_key.get()));
  EXPECT_EQ(SSL_get_certificate(ssl.get()), certificate);

  // SSL_use_certificate / SSL_use_PrivateKey take their own references, so
  // releasing the caller's references leaves the connection's identity alive
  // (and SSL_free later releases the connection's reference).
  identity.certificate.reset();
  identity.private_key.reset();
  EXPECT_EQ(SSL_get_certificate(ssl.get()), certificate);
}

TEST(SslCallbackUnitTest, context_view_installs_identity_from_memory) {
  test_identity identity = parse_test_identity();

  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  EXPECT_FALSE(ctx.view().use_certificate(identity.certificate.get()));
  EXPECT_FALSE(ctx.view().use_private_key(identity.private_key.get()));
  // The installed certificate and key now match on the native context.
  EXPECT_FALSE(ctx.view().check_private_key());

  // The owning forwarders reach the same installers.
  context owner(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(owner.valid());
  EXPECT_FALSE(owner.use_certificate(identity.certificate.get()));
  EXPECT_FALSE(owner.use_private_key(identity.private_key.get()));
  EXPECT_FALSE(owner.check_private_key());
}

TEST(SslCallbackUnitTest, context_view_reports_openssl_install_failures) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());

  const std::error_code certificate_error = ctx.view().use_certificate(nullptr);
  EXPECT_TRUE(certificate_error);
  EXPECT_EQ(certificate_error.category(), bnio::ssl::openssl_error_category());
  EXPECT_NE(certificate_error, bnio::ssl::make_no_ssl_error());
  EXPECT_FALSE(certificate_error.message().empty());
  const std::error_code private_key_error = ctx.view().use_private_key(nullptr);
  EXPECT_TRUE(private_key_error);
  EXPECT_EQ(private_key_error.category(), bnio::ssl::openssl_error_category());
  EXPECT_NE(private_key_error, bnio::ssl::make_no_ssl_error());
  EXPECT_FALSE(private_key_error.message().empty());
}

// ---------------------------------------------------------------------------
// cert_closure trampoline
// ---------------------------------------------------------------------------

static_assert(static_cast<int>(bnio::ssl::cert_result::fail) == 0);
static_assert(static_cast<int>(bnio::ssl::cert_result::ok) == 1);
static_assert(static_cast<int>(bnio::ssl::cert_result::retry) == -1);

struct cert_trampoline_probe {
  bool invoked = false;
  int calls = 0;
  bnio::ssl::cert_result result = bnio::ssl::cert_result::ok;
  X509* certificate = nullptr;
  EVP_PKEY* private_key = nullptr;
  context_view seen_context;
  SSL* seen_ssl = nullptr;
  std::error_code certificate_error;
  std::error_code private_key_error;
};

// Bound callable for the cert trampoline: the probe is bound by reference and
// carries the identity to install plus the observations.
struct cert_trampoline_callable {
  bnio::ssl::cert_result operator()(context_view ctx, stream_view ssl,
                                    bnio::ssl::cert_out& out,
                                    cert_trampoline_probe& probe) {
    probe.invoked = true;
    ++probe.calls;
    probe.seen_context = ctx;
    probe.seen_ssl = ssl.native_handle();
    if (probe.certificate != nullptr) {
      probe.certificate_error = out.use_certificate(probe.certificate);
    }
    if (probe.private_key != nullptr) {
      probe.private_key_error = out.use_private_key(probe.private_key);
    }
    return probe.result;
  }
};

using cert_trampoline_closure =
    detail::cert_closure<bnio::ssl::cert_out, std::allocator<std::byte>,
                         cert_trampoline_callable,
                         std::reference_wrapper<cert_trampoline_probe>>;

TEST(SslCallbackUnitTest, cert_trampoline_maps_every_result_to_the_wire_value) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  test_identity identity = parse_test_identity();
  cert_trampoline_probe probe;
  probe.certificate = identity.certificate.get();
  probe.private_key = identity.private_key.get();
  std::unique_ptr<cert_trampoline_closure,
                  closure_deleter<cert_trampoline_closure>>
      closure(detail::make_closure<cert_trampoline_closure>(
          std::allocator<std::byte>{}, cert_trampoline_callable{},
          std::ref(probe)));

  struct mapping {
    bnio::ssl::cert_result result;
    int wire;
  };
  const mapping mappings[] = {
      {bnio::ssl::cert_result::ok, 1},
      {bnio::ssl::cert_result::fail, 0},
      {bnio::ssl::cert_result::retry, -1},
  };
  for (const mapping& entry : mappings) {
    probe.result = entry.result;
    EXPECT_EQ(closure->trampoline(ssl.get(), closure.get()), entry.wire);
  }
  EXPECT_EQ(probe.calls, 3);
  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.seen_context.native_handle(), ctx.native_handle());
  EXPECT_EQ(probe.seen_ssl, ssl.get());
  EXPECT_FALSE(probe.certificate_error);
  EXPECT_FALSE(probe.private_key_error);
  EXPECT_EQ(SSL_get_certificate(ssl.get()), identity.certificate.get());
}

struct throwing_cert_callable {
  bnio::ssl::cert_result operator()(context_view, stream_view,
                                    bnio::ssl::cert_out&) const {
    throw std::runtime_error("cert callback threw");
  }
};

using throwing_cert_closure =
    detail::cert_closure<bnio::ssl::cert_out, std::allocator<std::byte>,
                         throwing_cert_callable>;

TEST(SslCallbackUnitTest, cert_trampoline_maps_exceptions_to_fail) {
  context ctx(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(ctx.valid());
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.native_handle()),
                                                &SSL_free);
  ASSERT_NE(ssl, nullptr);

  std::unique_ptr<throwing_cert_closure, closure_deleter<throwing_cert_closure>>
      closure(detail::make_closure<throwing_cert_closure>(
          std::allocator<std::byte>{}, throwing_cert_callable{}));

  // No exception may cross the OpenSSL boundary: the fallback aborts the
  // handshake (callback return value 0).
  EXPECT_EQ(closure->trampoline(ssl.get(), closure.get()), 0);
}

// ---------------------------------------------------------------------------
// Concept acceptance for the new callback channels
// ---------------------------------------------------------------------------

template <class ContextArg>
struct servername_context_arg_callback {
  int operator()(ContextArg, stream_view, bnio::ssl::sni_out&) const;
};

template <class ContextArg>
concept servername_concept_accepts_context_arg =
    detail::servername_callback_invocable<
        servername_context_arg_callback<ContextArg>>;

static_assert(servername_concept_accepts_context_arg<context_view>);
static_assert(servername_concept_accepts_context_arg<const context_view&>);
static_assert(servername_concept_accepts_context_arg<context_view&>);
static_assert(!servername_concept_accepts_context_arg<context_view&&>);

struct void_servername_callback {
  void operator()(context_view, stream_view, bnio::ssl::sni_out&) const;
};
static_assert(!detail::servername_callback_invocable<void_servername_callback>);

template <class ContextArg>
struct client_hello_context_arg_callback {
  bnio::ssl::client_hello_result operator()(ContextArg, stream_view,
                                            bnio::ssl::alert_out&) const;
};

template <class ContextArg>
concept client_hello_concept_accepts_context_arg =
    detail::client_hello_callback_invocable<
        client_hello_context_arg_callback<ContextArg>>;

static_assert(client_hello_concept_accepts_context_arg<context_view>);
static_assert(client_hello_concept_accepts_context_arg<const context_view&>);
static_assert(client_hello_concept_accepts_context_arg<context_view&>);
static_assert(!client_hello_concept_accepts_context_arg<context_view&&>);

struct int_client_hello_callback {
  int operator()(context_view, stream_view, bnio::ssl::alert_out&) const;
};
static_assert(
    !detail::client_hello_callback_invocable<int_client_hello_callback>);

template <class ContextArg>
struct cert_context_arg_callback {
  bnio::ssl::cert_result operator()(ContextArg, stream_view,
                                    bnio::ssl::cert_out&) const;
};

template <class ContextArg>
concept cert_concept_accepts_context_arg =
    detail::cert_callback_invocable<cert_context_arg_callback<ContextArg>>;

static_assert(cert_concept_accepts_context_arg<context_view>);
static_assert(cert_concept_accepts_context_arg<const context_view&>);
static_assert(cert_concept_accepts_context_arg<context_view&>);
static_assert(!cert_concept_accepts_context_arg<context_view&&>);

struct int_cert_callback {
  int operator()(context_view, stream_view, bnio::ssl::cert_out&) const;
};
static_assert(!detail::cert_callback_invocable<int_cert_callback>);

// The public overloads apply the same constraints, so a callable that cannot
// be invoked with an lvalue context view is rejected at overload resolution.
template <class ContextArg>
concept set_servername_callback_accepts = requires(
    context& ctx, servername_context_arg_callback<ContextArg> callback) {
  ctx.set_servername_callback(callback);
};

static_assert(set_servername_callback_accepts<context_view>);
static_assert(!set_servername_callback_accepts<context_view&&>);

template <class ContextArg>
concept set_client_hello_callback_accepts = requires(
    context& ctx, client_hello_context_arg_callback<ContextArg> callback) {
  ctx.set_client_hello_callback(callback);
};

static_assert(set_client_hello_callback_accepts<context_view>);
static_assert(!set_client_hello_callback_accepts<context_view&&>);

template <class ContextArg>
concept set_cert_callback_accepts =
    requires(context& ctx, cert_context_arg_callback<ContextArg> callback) {
      ctx.set_cert_callback(callback);
    };

static_assert(set_cert_callback_accepts<context_view>);
static_assert(!set_cert_callback_accepts<context_view&&>);

// ---------------------------------------------------------------------------
// Raw setter forwarding signatures (context_base -> context_view, context)
// ---------------------------------------------------------------------------

template <class MemPtr>
struct member_signature;

template <class C, class R, class... Args>
struct member_signature<R (C::*)(Args...)> {
  using result = R;
  using arguments = std::tuple<Args...>;
  static constexpr bool is_noexcept = false;
};

template <class C, class R, class... Args>
struct member_signature<R (C::*)(Args...) noexcept> {
  using result = R;
  using arguments = std::tuple<Args...>;
  static constexpr bool is_noexcept = true;
};

template <class C, class R, class... Args>
struct member_signature<R (C::*)(Args...) const> {
  using result = R;
  using arguments = std::tuple<Args...>;
  static constexpr bool is_noexcept = false;
};

template <class C, class R, class... Args>
struct member_signature<R (C::*)(Args...) const noexcept> {
  using result = R;
  using arguments = std::tuple<Args...>;
  static constexpr bool is_noexcept = true;
};

template <class OwnerPtr, class ViewPtr>
constexpr bool same_member_signature =
    std::is_same_v<typename member_signature<OwnerPtr>::result,
                   typename member_signature<ViewPtr>::result> &&
    std::is_same_v<typename member_signature<OwnerPtr>::arguments,
                   typename member_signature<ViewPtr>::arguments> &&
    member_signature<OwnerPtr>::is_noexcept ==
        member_signature<ViewPtr>::is_noexcept;

#define BNIO_ASSERT_FORWARDS(owner, name)                                    \
  static_assert(                                                             \
      same_member_signature<decltype(&owner::name),                          \
                            decltype(&bnio::ssl::base::context_view::name)>, \
      #owner "::" #name " must match context_view::" #name)

BNIO_ASSERT_FORWARDS(bnio::ssl::base::context_base,
                     set_tlsext_servername_callback);
BNIO_ASSERT_FORWARDS(bnio::ssl::base::context_base, set_tlsext_servername_arg);
BNIO_ASSERT_FORWARDS(bnio::ssl::base::context_base, set_client_hello_cb);
BNIO_ASSERT_FORWARDS(bnio::ssl::base::context_base, set_cert_cb);
BNIO_ASSERT_FORWARDS(bnio::ssl::base::context_base, use_certificate);
BNIO_ASSERT_FORWARDS(bnio::ssl::base::context_base, use_private_key);
BNIO_ASSERT_FORWARDS(bnio::ssl::context, set_tlsext_servername_callback);
BNIO_ASSERT_FORWARDS(bnio::ssl::context, set_tlsext_servername_arg);
BNIO_ASSERT_FORWARDS(bnio::ssl::context, set_client_hello_cb);
BNIO_ASSERT_FORWARDS(bnio::ssl::context, set_cert_cb);
BNIO_ASSERT_FORWARDS(bnio::ssl::context, use_certificate);
BNIO_ASSERT_FORWARDS(bnio::ssl::context, use_private_key);

#undef BNIO_ASSERT_FORWARDS

static_assert(std::is_same_v<bnio::ssl::context::servername_cb,
                             bnio::ssl::base::context_view::servername_cb>);
static_assert(std::is_same_v<bnio::ssl::context::client_hello_cb,
                             bnio::ssl::base::context_view::client_hello_cb>);
static_assert(std::is_same_v<bnio::ssl::context::cert_cb,
                             bnio::ssl::base::context_view::cert_cb>);
static_assert(std::is_same_v<bnio::ssl::base::context_base::servername_cb,
                             bnio::ssl::base::context_view::servername_cb>);
static_assert(std::is_same_v<bnio::ssl::base::context_base::client_hello_cb,
                             bnio::ssl::base::context_view::client_hello_cb>);
static_assert(std::is_same_v<bnio::ssl::base::context_base::cert_cb,
                             bnio::ssl::base::context_view::cert_cb>);

}  // namespace
