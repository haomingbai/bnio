#pragma once
#ifndef BUPP_EXAMPLES_MINI_CURL_OPERATION_REGISTRY_HPP_
#define BUPP_EXAMPLES_MINI_CURL_OPERATION_REGISTRY_HPP_

#include <bexec/bexec.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace mini_curl {

/**
 * Type-erased base for operations stored by operation_registry.
 */
struct operation_holder_base {
  /**
   * Destroys a stored operation holder.
   */
  virtual ~operation_holder_base() = default;

  /**
   * Starts the wrapped async operation.
   */
  virtual void start() noexcept = 0;
};

/**
 * Thread-local registry that keeps async operations alive until they complete.
 * Operations are cleaned up by reap() after the event loop stops.
 */
class operation_registry {
 public:
  /**
   * Connects a sender to a receiver, starts it, and keeps the operation alive.
   */
  template <class Sender, class Receiver>
  void spawn(Sender&& sender, Receiver&& receiver) {
    using operation_type = decltype(bexec::connect(std::declval<Sender>(),
                                                   std::declval<Receiver>()));

    struct holder final : operation_holder_base {
      operation_type operation;

      holder(Sender&& s, Receiver&& r)
          : operation(bexec::connect(std::forward<Sender>(s),
                                     std::forward<Receiver>(r))) {}

      void start() noexcept override { bexec::start(operation); }
    };

    auto op = std::make_unique<holder>(std::forward<Sender>(sender),
                                       std::forward<Receiver>(receiver));
    op->start();
    ops_.push_back(std::move(op));
  }

  /**
   * Releases all stored async operations.
   */
  void clear() noexcept { ops_.clear(); }

 private:
  std::vector<std::unique_ptr<operation_holder_base>> ops_;
};

}  // namespace mini_curl

#endif  // BUPP_EXAMPLES_MINI_CURL_OPERATION_REGISTRY_HPP_
