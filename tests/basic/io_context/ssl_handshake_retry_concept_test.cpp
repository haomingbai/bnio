/**
 * @file ssl_handshake_retry_concept_test.cpp
 * @brief Concept tests for the factory-driven SSL handshake retry surface.
 *
 * These tests include detail headers on purpose: the factory/sender concepts
 * are internal classification tools, not public API.
 */

#include <bnio/ssl.h>
#include <bnio/ssl/detail/common.h>
#include <bnio/ssl/detail/handshake_concepts.h>
#include <bnio/ssl/detail/handshake_retry.h>
#include <bnio/ssl/detail/senders.h>
#include <gtest/gtest.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/just.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <exception>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {

using stream_type = bnio::ssl::tcp::stream<bnio::tcp_socket>;
using scheduler_type = bnio::io_context::post_scheduler;
using bnio::ssl::handshake_type;
using bnio::ssl::detail::handshake_retry_factory;

/** Receiver matching the public retry contract. */
struct retry_test_receiver {
  void set_value(std::error_code) noexcept {}
  void set_stopped() noexcept {}
};

/** The old 2-argument handshake sender stays available and unchanged. */
using old_sender_type = decltype(std::declval<stream_type&>().async_handshake(
    std::declval<scheduler_type>(), std::declval<bnio::ssl::handshake_type>()));
using old_completions = bexec::completion_signatures_of_t<old_sender_type>;

static_assert(
    std::same_as<old_sender_type, bnio::ssl::detail::handshake_sender<
                                      scheduler_type, bnio::tcp_socket>>,
    "the 2-argument async_handshake sender type must not change");
static_assert(
    std::same_as<old_completions, bexec::completion_signatures<
                                      bexec::set_value_t(std::error_code),
                                      bexec::set_stopped_t()>>,
    "the 2-argument handshake completion contract must not change");
static_assert(bexec::sender<old_sender_type>);

/** Sender type produced by the 3-argument async_handshake. */
template <class Factory>
using retry_sender_type = decltype(std::declval<stream_type&>().async_handshake(
    std::declval<scheduler_type>(), std::declval<handshake_type>(),
    std::declval<Factory>()));

// A factory passed by value: callable on a non-const lvalue, returns a
// synchronous one-value sender.
struct by_value_factory {
  auto operator()(const std::error_code&) const {
    return bexec::just(std::error_code{});
  }
};

// A factory whose call operator only accepts a const lvalue reference, which
// is exactly the invocation value category used by the implementation.
struct const_ref_factory {
  auto operator()(const std::error_code&) const {
    return bexec::just(std::error_code{});
  }
};

// A mutable-only factory: still invocable as an lvalue.
struct mutable_factory {
  auto operator()(const std::error_code&) {
    return bexec::just(std::error_code{});
  }
};

// A move-only factory: accepted, with &&-only connect on the sender.
struct move_only_factory {
  move_only_factory() = default;
  move_only_factory(move_only_factory&&) = default;
  move_only_factory& operator=(move_only_factory&&) = default;
  move_only_factory(const move_only_factory&) = delete;
  move_only_factory& operator=(const move_only_factory&) = delete;

  auto operator()(const std::error_code&) const {
    return bexec::just(std::error_code{});
  }
};

// Rejected: only accepts an rvalue error code.
struct rvalue_only_factory {
  auto operator()(std::error_code&&) const {
    return bexec::just(std::error_code{});
  }
};

// Rejected: returns a reference instead of a value.
struct reference_result_factory {
  std::error_code& operator()(const std::error_code&) const;
};

// Rejected: advertises an error channel.
struct error_channel_sender {
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_error_t(std::exception_ptr)>;
};
struct error_channel_factory {
  error_channel_sender operator()(const std::error_code&) const { return {}; }
};

// Rejected: advertises two value signatures.
struct two_value_sender {
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_value_t(std::error_code)>;
};
struct two_value_factory {
  two_value_sender operator()(const std::error_code&) const { return {}; }
};

// Rejected: not a sender at all.
struct non_sender {};
struct non_sender_factory {
  non_sender operator()(const std::error_code&) const { return {}; }
};

// A well-formed synchronous sender used by the dual-overload factory below.
struct value_step_sender {
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;

  template <class Receiver>
  struct operation {
    Receiver receiver;

    void start() noexcept {
      bexec::set_value(std::move(receiver), std::error_code{});
    }
  };

  template <class Receiver>
  operation<Receiver> connect(Receiver receiver) && {
    return operation<Receiver>{std::move(receiver)};
  }
};

// Regression for the factory invocation value category: the const& overload
// is the one the concept accepts, so it must also be the one the operation
// invokes. The non-const overload returns a different (non-sender) type, so
// instantiating the operation would fail outright if the implementation used
// a non-const lvalue.
struct dual_overload_factory {
  value_step_sender operator()(const std::error_code&) const { return {}; }
  int operator()(std::error_code&) const { return 0; }
};

// ---- accepted factories -------------------------------------------------

static_assert(handshake_retry_factory<by_value_factory>);
static_assert(handshake_retry_factory<const_ref_factory>);
static_assert(handshake_retry_factory<mutable_factory>);
static_assert(handshake_retry_factory<move_only_factory>);
static_assert(handshake_retry_factory<by_value_factory&>);
static_assert(handshake_retry_factory<const by_value_factory&>);
static_assert(handshake_retry_factory<by_value_factory&&>);

// The dual-overload factory is accepted through its const& overload, and the
// resulting sender connects successfully. This is a compile-time regression:
// with a non-const invocation the operation would instantiate
// bexec::connect(int, retry_receiver) and fail here.
static_assert(handshake_retry_factory<dual_overload_factory>);
using dual_sender = retry_sender_type<dual_overload_factory>;
static_assert(bexec::operation_state<
              decltype(bexec::connect(std::declval<dual_sender>(),
                                      std::declval<retry_test_receiver>()))>);

// A fully noexcept factory (invocation, sender move, and connect) instantiates
// the retry operation's nothrow builder branch; without a noexcept factory in
// the suite that branch would never be compiled.
struct noexcept_step_factory {
  auto operator()(const std::error_code&) const noexcept {
    return bexec::just(std::error_code{});
  }
};
static_assert(handshake_retry_factory<noexcept_step_factory>);
using noexcept_operation = bnio::ssl::detail::handshake_retry_operation<
    scheduler_type, bnio::tcp_socket, retry_test_receiver,
    noexcept_step_factory>;
static_assert(noexcept_operation::retry_nothrow,
              "the noexcept factory must compile the nothrow builder branch");

// ---- rejected factories -------------------------------------------------

static_assert(!handshake_retry_factory<rvalue_only_factory>);
static_assert(!handshake_retry_factory<reference_result_factory>);
static_assert(!handshake_retry_factory<error_channel_factory>);
static_assert(!handshake_retry_factory<two_value_factory>);
static_assert(!handshake_retry_factory<non_sender_factory>);

// ---- the public retry sender contract -----------------------------------

using by_value_sender = retry_sender_type<by_value_factory>;
using by_value_completions = bexec::completion_signatures_of_t<by_value_sender>;

static_assert(bexec::sender<by_value_sender>);
static_assert(
    std::same_as<by_value_completions, bexec::completion_signatures<
                                           bexec::set_value_t(std::error_code),
                                           bexec::set_stopped_t()>>,
    "the retry sender must advertise exactly value(error_code) and "
    "stopped");
static_assert(by_value_completions::template count_of<bexec::set_value_t>() ==
              1);
static_assert(by_value_completions::template count_of<bexec::set_stopped_t>() ==
              1);
static_assert(by_value_completions::template count_of<bexec::set_error_t>() ==
              0);
static_assert(bexec::receiver_of<retry_test_receiver, by_value_completions>);
static_assert(std::same_as<by_value_completions, old_completions>,
              "the retry sender keeps the unchanged public receiver contract");

// A prvalue connect is always available, including for a move-only factory.
static_assert(bexec::operation_state<
              decltype(bexec::connect(std::declval<by_value_sender>(),
                                      std::declval<retry_test_receiver>()))>);
using move_only_sender = retry_sender_type<move_only_factory>;
static_assert(bexec::operation_state<
              decltype(bexec::connect(std::declval<move_only_sender>(),
                                      std::declval<retry_test_receiver>()))>);

// Lvalue connect requires a copyable scheduler and factory.
static_assert(bexec::operation_state<
              decltype(bexec::connect(std::declval<by_value_sender&>(),
                                      std::declval<retry_test_receiver>()))>);
static_assert(!std::copy_constructible<move_only_factory>);

// Named concept instead of an inline requires-expression: some compilers
// report the nested constraint failure of an inline requires-expression as a
// hard error in this position.
template <class Sender>
concept lvalue_connectable =
    requires(Sender& sender, retry_test_receiver receiver) {
      sender.connect(std::move(receiver));
    };

static_assert(lvalue_connectable<by_value_sender>);
static_assert(!lvalue_connectable<move_only_sender>);

// The 4-argument customization point object forwards to the retry member.
using cpo_sender = decltype(bnio::ssl::async_handshake(
    std::declval<scheduler_type>(), std::declval<stream_type&>(),
    std::declval<handshake_type>(), std::declval<by_value_factory>()));
static_assert(std::same_as<cpo_sender, by_value_sender>);

// The 3-argument customization point object still forwards to the old member.
using old_cpo_sender = decltype(bnio::ssl::async_handshake(
    std::declval<scheduler_type>(), std::declval<stream_type&>(),
    std::declval<handshake_type>()));
static_assert(std::same_as<old_cpo_sender, old_sender_type>);

TEST(SslHandshakeRetryConceptTest, factory_acceptance_matches_static_asserts) {
  EXPECT_TRUE(handshake_retry_factory<by_value_factory>);
  EXPECT_TRUE(handshake_retry_factory<const_ref_factory>);
  EXPECT_TRUE(handshake_retry_factory<mutable_factory>);
  EXPECT_TRUE(handshake_retry_factory<move_only_factory>);
  EXPECT_TRUE(handshake_retry_factory<dual_overload_factory>);

  EXPECT_FALSE(handshake_retry_factory<rvalue_only_factory>);
  EXPECT_FALSE(handshake_retry_factory<reference_result_factory>);
  EXPECT_FALSE(handshake_retry_factory<error_channel_factory>);
  EXPECT_FALSE(handshake_retry_factory<two_value_factory>);
  EXPECT_FALSE(handshake_retry_factory<non_sender_factory>);
}

TEST(SslHandshakeRetryConceptTest, sender_contract_counts) {
  EXPECT_EQ(by_value_completions::count_of<bexec::set_value_t>(),
            std::size_t{1});
  EXPECT_EQ(by_value_completions::count_of<bexec::set_stopped_t>(),
            std::size_t{1});
  EXPECT_EQ(by_value_completions::count_of<bexec::set_error_t>(),
            std::size_t{0});
}

// ---------------------------------------------------------------------------
// The defensive resume() fallback: operation_base only forwards finish, fail,
// and pause to the retry operation. Exercise the unexpected-action branch
// directly through the detail operation so it is not dead code and stays a
// terminal, visible protocol violation.
// ---------------------------------------------------------------------------

struct fallback_record {
  unsigned values = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
  std::error_code error;
};

struct fallback_receiver {
  fallback_record* record = nullptr;
  bnio::io_context* context = nullptr;

  void set_value(std::error_code ec) noexcept {
    record->error = ec;
    if (ec) {
      ++record->errors;
    } else {
      ++record->values;
    }
    stop_context();
  }

  void set_stopped() noexcept {
    ++record->stopped;
    stop_context();
  }

 private:
  void stop_context() noexcept {
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct fallback_factory {
  auto operator()(const std::error_code&) const {
    return bexec::just(std::error_code{});
  }
};

TEST(SslHandshakeRetryConceptTest, resume_fallback_reports_protocol_error) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();

  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  // The fallback must not touch the stream or run OpenSSL, so a moved-from
  // stream is enough.
  stream_type source{bnio::tcp_socket(-1), ssl_context};
  stream_type owner{std::move(source)};
  ASSERT_FALSE(source.valid());

  fallback_record record;
  using operation_type = bnio::ssl::detail::handshake_retry_operation<
      scheduler_type, bnio::tcp_socket, fallback_receiver, fallback_factory>;

  operation_type operation(
      scheduler, source, handshake_type::client, fallback_factory{},
      fallback_receiver{&record, &context});

  // No other resume_action reaches the retry operation in production; feed
  // one directly and verify the contract-preserving terminal.
  operation.resume(bnio::ssl::detail::resume_action::handshake);
  context.run();

  EXPECT_EQ(record.values, 0U);
  EXPECT_EQ(record.errors, 1U);
  EXPECT_EQ(record.stopped, 0U);
  EXPECT_EQ(record.error, std::make_error_code(std::errc::protocol_error));
}

}  // namespace
