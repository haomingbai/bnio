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
 * RAII wrapper for an io_uring operation probe.
 *
 * @see io_uring_probe
 */
class BNIO_EXPORT probe {
 public:
  /**
   * Creates an empty probe wrapper.
   *
   * @see io_uring_probe
   */
  probe() noexcept;

  /**
   * Frees the owned probe, if any.
   *
   * @see io_uring_free_probe
   */
  ~probe() noexcept;

  /**
   * Copy construction is disabled because probe owns a probe allocation.
   */
  probe(const probe&) = delete;

  /**
   * Copy assignment is disabled because probe owns a probe allocation.
   */
  probe& operator=(const probe&) = delete;

  /**
   * Moves ownership of a probe.
   *
   * @see io_uring_probe
   */
  probe(probe&& other) noexcept;

  /**
   * Moves ownership of a probe.
   *
   * @see io_uring_probe
   */
  probe& operator=(probe&& other) noexcept;

  /**
   * Allocates a probe by creating a temporary ring.
   *
   * @see io_uring_get_probe
   */
  io_uring_probe* get_probe() noexcept;

  /**
   * Allocates a probe using an existing ring.
   *
   * @see io_uring_get_probe_ring
   */
  io_uring_probe* get_probe_ring(ring& source) noexcept;

  /**
   * Frees the owned probe.
   *
   * @see io_uring_free_probe
   */
  void free_probe() noexcept;

  /**
   * Returns whether the probed ring supports an opcode.
   *
   * @see io_uring_opcode_supported
   */
  [[nodiscard]] int opcode_supported(int op) const noexcept;

  /**
   * Returns the wrapped probe.
   *
   * @see io_uring_probe
   */
  [[nodiscard]] io_uring_probe* raw() noexcept;

  /**
   * Returns the wrapped probe.
   *
   * @see io_uring_probe
   */
  [[nodiscard]] const io_uring_probe* raw() const noexcept;

  /**
   * Returns whether this wrapper currently owns a probe.
   *
   * @see io_uring_probe
   */
  [[nodiscard]] bool is_open() const noexcept;

 private:
  io_uring_probe* probe_ = nullptr;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_LINUX_PROBE_H_
