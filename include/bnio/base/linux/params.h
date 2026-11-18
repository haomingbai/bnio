/**
 * @file params.h
 * @brief Wrapper for io_uring_params.
 */

#pragma once
#ifndef BNIO_BASE_LINUX_PARAMS_H_
#define BNIO_BASE_LINUX_PARAMS_H_

#include <bnio/export.h>
#include <liburing.h>

#include <cstdint>

namespace bnio::base {

/**
 * Owning inline io_uring_params value; copies and moves duplicate the value
 * and share no kernel resources.
 */
class BNIO_EXPORT params {
 public:
  /** Creates zero-initialized io_uring parameters. */
  params() noexcept;

  /** Copies the parameter value. */
  params(const params&) noexcept = default;

  /** Copy assigns the parameter value. */
  params& operator=(const params&) noexcept = default;

  /** Moves the parameter value. */
  params(params&&) noexcept = default;

  /** Move assigns the parameter value. */
  params& operator=(params&&) noexcept = default;

  /** Destroys the parameter value. */
  ~params() noexcept = default;

  /** Returns the wrapped parameter structure. */
  [[nodiscard]] io_uring_params* raw() noexcept;

  /** Const overload. */
  [[nodiscard]] const io_uring_params* raw() const noexcept;

  /** Resets all parameter fields to zero. */
  void reset() noexcept;

  /** Submission queue entry count. */
  [[nodiscard]] std::uint32_t sq_entries() const noexcept;

  /** Sets the submission queue entry count. */
  void set_sq_entries(std::uint32_t sq_entries) noexcept;

  /** Completion queue entry count. */
  [[nodiscard]] std::uint32_t cq_entries() const noexcept;

  /** Sets the completion queue entry count. */
  void set_cq_entries(std::uint32_t cq_entries) noexcept;

  /** io_uring setup flags. */
  [[nodiscard]] std::uint32_t flags() const noexcept;

  /** Sets the io_uring setup flags. */
  void set_flags(std::uint32_t flags) noexcept;

  /** SQPOLL thread CPU. */
  [[nodiscard]] std::uint32_t sq_thread_cpu() const noexcept;

  /** Sets the SQPOLL thread CPU. */
  void set_sq_thread_cpu(std::uint32_t sq_thread_cpu) noexcept;

  /** SQPOLL idle timeout. */
  [[nodiscard]] std::uint32_t sq_thread_idle() const noexcept;

  /** Sets the SQPOLL idle timeout. */
  void set_sq_thread_idle(std::uint32_t sq_thread_idle) noexcept;

  /** Kernel-reported feature flags. */
  [[nodiscard]] std::uint32_t features() const noexcept;

  /** Sets the feature flags. */
  void set_features(std::uint32_t features) noexcept;

  /** Worker queue file descriptor. */
  [[nodiscard]] std::uint32_t wq_fd() const noexcept;

  /** Sets the worker queue file descriptor. */
  void set_wq_fd(std::uint32_t wq_fd) noexcept;

 private:
  io_uring_params params_{};
};

}  // namespace bnio::base

#endif  // BNIO_BASE_LINUX_PARAMS_H_
