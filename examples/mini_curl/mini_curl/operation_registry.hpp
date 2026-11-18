#pragma once
#ifndef BNIO_EXAMPLES_MINI_CURL_OPERATION_REGISTRY_HPP_
#define BNIO_EXAMPLES_MINI_CURL_OPERATION_REGISTRY_HPP_

#include <bexec/bexec.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace mini_curl {

/** Type-erased base for operations stored by operation_registry. */
struct operation_holder_base {
  /** Destroys the holder and its operation. */
  virtual ~operation_holder_base() = default;

  /** Starts the wrapped operation. */
  virtual void start() noexcept = 0;
};

/** Thread-local registry that keeps async operations alive until clear(). */
class operation_registry {
 public:
  /** Connects, starts, and stores the operation. */
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

  /** Releases all stored operations. */
  void clear() noexcept { ops_.clear(); }

 private:
  std::vector<std::unique_ptr<operation_holder_base>> ops_;
};

}  // namespace mini_curl

#endif  // BNIO_EXAMPLES_MINI_CURL_OPERATION_REGISTRY_HPP_
