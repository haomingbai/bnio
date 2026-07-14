#pragma once
#ifndef BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_RESOLVE_H_
#define BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_RESOLVE_H_

#include <bupp/async_io/bsd/kqueue_context_base.h>
#include <bupp/async_io/dns.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp::async_io::bsd_native {

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
                           bupp::async_io::dns_query query,
                           bupp::async_io::dns_result_view result,
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
   * Runs the async_io platform resolver helper.
   */
  void complete_resolve() noexcept {
    std::size_t count = 0;
    const std::error_code error =
        bupp::async_io::resolve_dns(query_, result_, count);
    if (error) {
      bexec::set_error(std::move(receiver_), error);
    } else {
      bexec::set_value(std::move(receiver_), count);
    }
  }

  kqueue_context* context_;
  bupp::async_io::dns_query query_;
  bupp::async_io::dns_result_view result_;
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
   */
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  /**
   * Creates a DNS sender for a context and query.
   */
  kqueue_resolve_sender(kqueue_context& context,
                        bupp::async_io::dns_query query,
                        bupp::async_io::dns_result_view result)
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
  bupp::async_io::dns_query query_;
  bupp::async_io::dns_result_view result_;
};

/** @cond BUPP_DETAIL */

inline auto kqueue_context::async_resolve(
    bupp::async_io::dns_query query, bupp::async_io::dns_result_view result) {
  return kqueue_resolve_sender(*this, std::move(query), result);
}

inline auto kqueue_context::async_resolve(
    std::string_view host, std::string_view service,
    bupp::async_io::dns_result_view result) {
  return async_resolve(bupp::async_io::dns_query(host, service), result);
}

/** @endcond */

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_RESOLVE_H_
