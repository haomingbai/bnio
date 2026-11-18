/**
 * @file probe.h
 * @brief Wrapper for io_uring_probe.
 */

#pragma once
#ifndef BNIO_BASE_LINUX_PROBE_H_
#define BNIO_BASE_LINUX_PROBE_H_

#include <bnio/export.h>

struct io_uring_probe;

namespace bnio::base {
class ring;
}  // namespace bnio::base

namespace bnio::base {

/**
 * RAII owner for an io_uring operation probe.
 *
 * Move-only; the allocation is freed with io_uring_free_probe.
 *
 * @see io_uring_probe
 */
class BNIO_EXPORT probe {
 public:
  /** Creates an empty probe wrapper. */
  probe() noexcept;

  /** Frees the owned probe. */
  ~probe() noexcept;

  /** Deleted: the wrapper owns a probe allocation. */
  probe(const probe&) = delete;

  /** Deleted: the wrapper owns a probe allocation. */
  probe& operator=(const probe&) = delete;

  /** Moves ownership of the probe. */
  probe(probe&& other) noexcept;

  /** Move assigns ownership of the probe. */
  probe& operator=(probe&& other) noexcept;

  /** Allocates a probe by creating a temporary ring. */
  io_uring_probe* get_probe() noexcept;

  /** Allocates a probe using @p source. */
  io_uring_probe* get_probe_ring(ring& source) noexcept;

  /** Frees the owned probe. */
  void free_probe() noexcept;

  /** Returns whether the probed ring supports @p op. */
  [[nodiscard]] int opcode_supported(int op) const noexcept;

  /** Returns the wrapped probe. */
  [[nodiscard]] io_uring_probe* raw() noexcept;

  /** Const overload. */
  [[nodiscard]] const io_uring_probe* raw() const noexcept;

  /** Returns whether a probe is currently owned. */
  [[nodiscard]] bool is_open() const noexcept;

 private:
  io_uring_probe* probe_ = nullptr;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_LINUX_PROBE_H_
