/**
 * @file resolve.h
 * @brief kqueue DNS resolve operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_RESOLVE_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_RESOLVE_H_

#include <bnio/async_io/bsd/kqueue_context_base.h>
#include <bnio/async_io/dns.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::bsd_native {

/**
 * Operation that posts a synchronous DNS query onto a kqueue_context.
 */
template <class Receiver>
class kqueue_resolve_operation : public kqueue_operation_base {
 public:
  /**
   * Creates a DNS resolution operation for a context and query.
   */
  kqueue_resolve_operation(kqueue_context& context,
                           bnio::async_io::dns_query query,
                           bnio::async_io::dns_result_view result,
                           Receiver receiver)
      : context_(&context),
        query_(std::move(query)),
        result_(result),
        receiver_(std::move(receiver)) {}

  /**
   * Copy construction is disabled because operations are queued intrusively.
   */
  kqueue_resolve_operation(const kqueue_resolve_operation&) = delete;

  /**
   * Copy assignment is disabled because operations are queued intrusively.
   */
  kqueue_resolve_operation& operator=(const kqueue_resolve_operation&) = delete;

  /**
   * Move construction is disabled because operations are queued intrusively.
   */
  kqueue_resolve_operation(kqueue_resolve_operation&&) = delete;

  /**
   * Move assignment is disabled because operations are queued intrusively.
   */
  kqueue_resolve_operation& operator=(kqueue_resolve_operation&&) = delete;

  /**
   * Destroys the operation without delivering an additional signal.
   */
  ~kqueue_resolve_operation() noexcept override = default;

  /**
   * Posts this operation to the context run loop.
   *
   * A stop-token cancel observed at start() selects the stopped channel
   * → set_stopped. Resolve runs on the CPU queue, which io_context::stop()
   * does not abort, so the stopped channel can only originate from this
   * pre-check.
   */
  void start() noexcept {
    if (stop_requested()) {
      completion_ = completion_kind::stopped;
      (void)context_->post(*this);
      return;
    }

    completion_ = completion_kind::value;
    (void)context_->post(*this);
  }

  /**
   * Executes the synchronous resolver and completes the receiver.
   */
  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        complete_resolve();
        break;
      case completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

 private:
  enum class completion_kind {
    value,
    stopped,
  };

  /**
   * Returns whether the receiver environment has requested cancellation.
   */
  [[nodiscard]] bool stop_requested() const noexcept {
    auto env = bexec::get_env(receiver_);
    auto token = bexec::query(env, bexec::get_stop_token);
    return token.stop_requested();
  }

  /**
   * Runs the async_io platform resolver helper. Both success and failure
   * exit through set_value(ec, count).
   */
  void complete_resolve() noexcept {
    std::size_t count = 0;
    const std::error_code ec =
        bnio::async_io::resolve_dns(query_, result_, count);
    bexec::set_value(std::move(receiver_), ec, count);
  }

  kqueue_context* context_;
  bnio::async_io::dns_query query_;
  bnio::async_io::dns_result_view result_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
};

/**
 * Sender returned by kqueue_context DNS resolution APIs.
 */
class kqueue_resolve_sender {
 public:
  /**
   * Completion signatures produced by a DNS resolution sender.
   *
   * set_value(ec, count) is the universal exit (success, resolver
   * failure); set_stopped is delivered when the receiver's stop token is
   * already requested at start() time. Resolve runs on the CPU queue,
   * which io_context::stop() does not abort, so no abort-path
   * set_value(operation_canceled) payload is produced here.
   */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t),
      bexec::set_stopped_t()>;

  /**
   * Creates a DNS sender for a context and query.
   */
  kqueue_resolve_sender(kqueue_context& context,
                        bnio::async_io::dns_query query,
                        bnio::async_io::dns_result_view result)
      : context_(&context), query_(std::move(query)), result_(result) {}

  /**
   * Connects an rvalue DNS sender to a receiver, moving the query.
   */
  template <class Receiver>
  auto connect(Receiver receiver) && {
    return kqueue_resolve_operation<std::remove_cvref_t<Receiver>>(
        *context_, std::move(query_), result_, std::move(receiver));
  }

  /**
   * Connects an lvalue DNS sender to a receiver, copying the query.
   */
  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return kqueue_resolve_operation<std::remove_cvref_t<Receiver>>(
        *context_, query_, result_, std::move(receiver));
  }

 private:
  kqueue_context* context_;
  bnio::async_io::dns_query query_;
  bnio::async_io::dns_result_view result_;
};

/** @cond BNIO_DETAIL */

inline auto kqueue_context::async_resolve(
    bnio::async_io::dns_query query, bnio::async_io::dns_result_view result) {
  return kqueue_resolve_sender(*this, std::move(query), result);
}

inline auto kqueue_context::async_resolve(
    std::string_view host, std::string_view service,
    bnio::async_io::dns_result_view result) {
  return async_resolve(bnio::async_io::dns_query(host, service), result);
}

/** @endcond */

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_RESOLVE_H_
