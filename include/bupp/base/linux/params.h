#pragma once
#ifndef BUPP_BASE_LINUX_PARAMS_H_
#define BUPP_BASE_LINUX_PARAMS_H_

#include <bupp/export.h>
#include <liburing.h>

#include <cstdint>

namespace bupp::base {

/**
 * Owning value wrapper for io_uring_params.
 *
 * The wrapper stores io_uring_params inline, so copying or moving duplicates
 * the parameter value and does not share kernel resources.
 *
 * @see io_uring_params
 */
class BUPP_EXPORT params {
 public:
  /**
   * Creates zero-initialized io_uring parameters.
   *
   * @see io_uring_params
   */
  params() noexcept;

  /**
   * Copies the parameter value.
   */
  params(const params&) noexcept = default;

  /**
   * Copies the parameter value.
   */
  params& operator=(const params&) noexcept = default;

  /**
   * Moves the parameter value.
   */
  params(params&&) noexcept = default;

  /**
   * Moves the parameter value.
   */
  params& operator=(params&&) noexcept = default;

  /**
   * Destroys the parameter value.
   */
  ~params() noexcept = default;

  /**
   * Returns the wrapped parameter structure.
   *
   * @see io_uring_params
   */
  [[nodiscard]] io_uring_params* raw() noexcept;

  /**
   * Returns the wrapped parameter structure.
   *
   * @see io_uring_params
   */
  [[nodiscard]] const io_uring_params* raw() const noexcept;

  /**
   * Resets all parameter fields to zero.
   *
   * @see io_uring_params
   */
  void reset() noexcept;

  /**
   * Returns the submission queue entry count.
   *
   * @see io_uring_params
   */
  [[nodiscard]] std::uint32_t sq_entries() const noexcept;

  /**
   * Sets the submission queue entry count field.
   *
   * @see io_uring_params
   */
  void set_sq_entries(std::uint32_t sq_entries) noexcept;

  /**
   * Returns the completion queue entry count.
   *
   * @see io_uring_params
   */
  [[nodiscard]] std::uint32_t cq_entries() const noexcept;

  /**
   * Sets the completion queue entry count field.
   *
   * @see io_uring_params
   */
  void set_cq_entries(std::uint32_t cq_entries) noexcept;

  /**
   * Returns io_uring setup flags.
   *
   * @see io_uring_params
   */
  [[nodiscard]] std::uint32_t flags() const noexcept;

  /**
   * Sets io_uring setup flags.
   *
   * @see io_uring_params
   */
  void set_flags(std::uint32_t flags) noexcept;

  /**
   * Returns the SQPOLL thread CPU field.
   *
   * @see io_uring_params
   */
  [[nodiscard]] std::uint32_t sq_thread_cpu() const noexcept;

  /**
   * Sets the SQPOLL thread CPU field.
   *
   * @see io_uring_params
   */
  void set_sq_thread_cpu(std::uint32_t sq_thread_cpu) noexcept;

  /**
   * Returns the SQPOLL idle timeout field.
   *
   * @see io_uring_params
   */
  [[nodiscard]] std::uint32_t sq_thread_idle() const noexcept;

  /**
   * Sets the SQPOLL idle timeout field.
   *
   * @see io_uring_params
   */
  void set_sq_thread_idle(std::uint32_t sq_thread_idle) noexcept;

  /**
   * Returns feature flags reported by the kernel.
   *
   * @see io_uring_params
   */
  [[nodiscard]] std::uint32_t features() const noexcept;

  /**
   * Sets the features field.
   *
   * @see io_uring_params
   */
  void set_features(std::uint32_t features) noexcept;

  /**
   * Returns the worker queue file descriptor field.
   *
   * @see io_uring_params
   */
  [[nodiscard]] std::uint32_t wq_fd() const noexcept;

  /**
   * Sets the worker queue file descriptor field.
   *
   * @see io_uring_params
   */
  void set_wq_fd(std::uint32_t wq_fd) noexcept;

 private:
  io_uring_params params_{};
};

}  // namespace bupp::base

#endif  // BUPP_BASE_LINUX_PARAMS_H_
