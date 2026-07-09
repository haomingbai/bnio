#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#error "This header is an io_uring_context class declaration fragment."
#endif

#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_SUBMISSION_API_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_SUBMISSION_API_H_

/**
 * Prepares an operation SQE without submitting the ring.
 */
template <class Operation>
int prepare(Operation& operation) noexcept;

/**
 * Submits all prepared SQEs on the context ring.
 *
 * @see io_uring_submit
 */
int submit() noexcept;

/**
 * Prepares one operation and submits the context ring.
 */
template <class Operation>
int submit(Operation& operation) noexcept;

/**
 * Runs a batch of submission work while holding the uring mutex once.
 *
 * The callback receives two callables: prepare(operation) reserves and fills
 * one SQE, and submit() submits all SQEs prepared so far.
 */
template <class Function>
void submit_batch(Function&& fn) noexcept;

// --- manual batch submission (caller holds uring_mutex_) ---

/**
 * Returns a lock guard for the io_uring mutex.
 */
[[nodiscard]] std::unique_lock<std::mutex> lock_uring() const noexcept {
  return std::unique_lock(uring_mutex_);
}

/**
 * Prepares one operation into the submission queue while the caller holds the
 * io_uring lock.
 */
template <class Operation>
int prepare_locked(Operation& operation) noexcept;

/**
 * Submits prepared queue entries while the caller holds the io_uring lock.
 */
int submit_locked() noexcept;

/**
 * Wakes threads waiting for io_uring progress.
 */
void notify_waiters() noexcept;

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_SUBMISSION_API_H_
