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
 * Runs a batch of submission work while holding the uring gate once.
 *
 * The callback receives two callables: prepare(operation) reserves and fills
 * one SQE, and submit() submits all SQEs prepared so far.
 */
template <class Function>
void submit_batch(Function&& fn) noexcept;

// --- manual batch submission (caller holds the uring gate) ---

/**
 * RAII guard for exclusive access to the io_uring SQ/CQ rings.
 */
class uring_lock {
 public:
  uring_lock() noexcept = default;

  uring_lock(const uring_lock&) = delete;
  uring_lock& operator=(const uring_lock&) = delete;

  uring_lock(uring_lock&& other) noexcept : gate_(other.gate_) {
    other.gate_ = nullptr;
  }

  uring_lock& operator=(uring_lock&& other) noexcept {
    if (this != &other) {
      reset();
      gate_ = other.gate_;
      other.gate_ = nullptr;
    }
    return *this;
  }

  ~uring_lock() noexcept { reset(); }

  [[nodiscard]] explicit operator bool() const noexcept {
    return gate_ != nullptr;
  }

  void reset() noexcept {
    if (gate_ != nullptr) {
      gate_->store(1U, std::memory_order_release);
      gate_ = nullptr;
    }
  }

 private:
  friend class io_uring_context;

  explicit uring_lock(std::atomic<unsigned>& gate) noexcept : gate_(&gate) {}

  std::atomic<unsigned>* gate_ = nullptr;
};

/**
 * Issues a short processor pause while spinning.
 */
static void pause_uring_spin() noexcept {
#if defined(__i386__) || defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

/**
 * Spins until exclusive io_uring access is acquired.
 */
[[nodiscard]] uring_lock lock_uring() const noexcept {
  for (;;) {
    if (uring_gate_.load(std::memory_order_acquire) != 0U &&
        uring_gate_.exchange(0U, std::memory_order_acq_rel) != 0U) {
      return uring_lock(uring_gate_);
    }
    while (uring_gate_.load(std::memory_order_relaxed) == 0U) {
      pause_uring_spin();
    }
  }
}

/**
 * Attempts to acquire exclusive io_uring access.
 */
[[nodiscard]] uring_lock try_lock_uring() const noexcept {
  if (uring_gate_.load(std::memory_order_acquire) == 0U ||
      uring_gate_.exchange(0U, std::memory_order_acq_rel) == 0U) {
    return uring_lock();
  }
  return uring_lock(uring_gate_);
}

/**
 * Prepares one operation into the submission queue while the caller holds the
 * io_uring gate.
 */
template <class Operation>
int prepare_locked(Operation& operation) noexcept;

/**
 * Submits prepared queue entries while the caller holds the io_uring gate.
 */
int submit_locked() noexcept;

/**
 * Wakes threads waiting for io_uring progress.
 */
void notify_waiters() noexcept;

/**
 * Wakes one thread waiting for io_uring progress.
 */
void notify_one_waiter() noexcept;

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_SUBMISSION_API_H_
