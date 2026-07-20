#include <asio.hpp>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark_common.h"

namespace {

class timer_churn_benchmark {
 public:
  timer_churn_benchmark(asio::io_context& context, timer_churn::config config)
      : context_(context), config_(config), timers_(config.timer_count) {}

  void start() {
    started_at_ = std::chrono::steady_clock::now();
    barrier_timer_.emplace(context_);

    for (std::size_t slot = 0; slot < config_.timer_count; ++slot) {
      timers_[slot].emplace(context_);
      ++metrics_.timers_created;
      if (timers_[slot]->expires_at(deadline(0, slot)) != 0) {
        fail();
        return;
      }
      ++metrics_.expiry_sets;
      start_active_wait(slot);
    }

    phase_ = phase::initial_barrier;
    start_barrier();
  }

  [[nodiscard]] bool successful() const noexcept {
    return finished_ && !failed_;
  }

  [[nodiscard]] const timer_churn::metrics& metrics() const noexcept {
    return metrics_;
  }

  [[nodiscard]] std::chrono::steady_clock::duration elapsed() const noexcept {
    return finished_at_ - started_at_;
  }

 private:
  enum class phase {
    initial_barrier,
    updating,
    destroying,
    finished,
  };

  static constexpr auto kInitialDeadline = std::chrono::hours{24 * 365};

  [[nodiscard]] asio::steady_timer::time_point deadline(
      std::size_t round, std::size_t slot) const noexcept {
    using milliseconds_rep = std::chrono::milliseconds::rep;
    using nanoseconds_rep = std::chrono::nanoseconds::rep;

    const auto round_offset =
        std::chrono::milliseconds{static_cast<milliseconds_rep>(round)};
    const auto slot_offset =
        std::chrono::nanoseconds{static_cast<nanoseconds_rep>(slot)};
    return started_at_ + kInitialDeadline - round_offset + slot_offset;
  }

  void start_active_wait(std::size_t slot) {
    timers_[slot]->async_wait([this](std::error_code error) {
      if (error == asio::error::operation_aborted) {
        on_active_wait_stopped();
      } else if (error) {
        fail();
      } else {
        fail();
      }
    });
    ++metrics_.waits_started;
  }

  void start_barrier() {
    if (barrier_timer_->expires_at(asio::steady_timer::clock_type::now()) !=
        0) {
      fail();
      return;
    }

    barrier_timer_->async_wait([this](std::error_code error) {
      if (error) {
        fail();
        return;
      }
      barrier_reached_ = true;
      advance_after_barrier();
    });
  }

  [[nodiscard]] bool is_replacement_slot(std::size_t slot) const noexcept {
    if (slot >= next_replacement_slot_) {
      return slot - next_replacement_slot_ < config_.replacements_per_round;
    }
    return config_.timer_count - next_replacement_slot_ + slot <
           config_.replacements_per_round;
  }

  void advance_replacement_window() noexcept {
    const std::size_t distance_to_end =
        config_.timer_count - next_replacement_slot_;
    if (config_.replacements_per_round >= distance_to_end) {
      next_replacement_slot_ = config_.replacements_per_round - distance_to_end;
    } else {
      next_replacement_slot_ += config_.replacements_per_round;
    }
  }

  void run_update_round() {
    phase_ = phase::updating;
    barrier_reached_ = false;
    expected_stopped_waits_ += config_.timer_count;
    const std::size_t round = completed_update_rounds_ + 1;

    for (std::size_t slot = 0; slot < config_.timer_count; ++slot) {
      const bool replace = is_replacement_slot(slot);
      if (replace) {
        if (timers_[slot]->cancel() != 1) {
          fail();
          return;
        }
        ++metrics_.explicit_cancels;
        timers_[slot].reset();
        ++metrics_.timers_destroyed;
        timers_[slot].emplace(context_);
        ++metrics_.timers_created;
      }

      const std::size_t canceled =
          timers_[slot]->expires_at(deadline(round, slot));
      if ((replace && canceled != 0) || (!replace && canceled != 1)) {
        fail();
        return;
      }
      ++metrics_.expiry_sets;
      start_active_wait(slot);
    }

    advance_replacement_window();
    start_barrier();
  }

  void on_active_wait_stopped() {
    ++metrics_.waits_stopped;
    if (phase_ == phase::destroying) {
      if (metrics_.waits_stopped == expected_stopped_waits_) {
        finish();
      }
      return;
    }

    advance_after_barrier();
  }

  void advance_after_barrier() {
    if (!barrier_reached_ ||
        metrics_.waits_stopped != expected_stopped_waits_) {
      return;
    }

    if (phase_ == phase::initial_barrier) {
      run_update_round();
      return;
    }

    if (phase_ != phase::updating) {
      fail();
      return;
    }

    ++completed_update_rounds_;
    if (completed_update_rounds_ == config_.update_rounds) {
      destroy_remaining_timers();
      return;
    }

    run_update_round();
  }

  void destroy_remaining_timers() {
    phase_ = phase::destroying;
    expected_stopped_waits_ += config_.timer_count;

    for (std::optional<asio::steady_timer>& timer : timers_) {
      if (timer->cancel() != 1) {
        fail();
        return;
      }
      ++metrics_.explicit_cancels;
      timer.reset();
      ++metrics_.timers_destroyed;
    }
  }

  void finish() {
    finished_at_ = std::chrono::steady_clock::now();
    finished_ = true;
    phase_ = phase::finished;
    context_.stop();
  }

  void fail() {
    failed_ = true;
    context_.stop();
  }

  asio::io_context& context_;
  timer_churn::config config_;
  timer_churn::metrics metrics_;
  std::vector<std::optional<asio::steady_timer>> timers_;
  std::optional<asio::steady_timer> barrier_timer_;
  std::chrono::steady_clock::time_point started_at_{};
  std::chrono::steady_clock::time_point finished_at_{};
  std::size_t expected_stopped_waits_ = 0;
  std::size_t completed_update_rounds_ = 0;
  std::size_t next_replacement_slot_ = 0;
  phase phase_ = phase::initial_barrier;
  bool barrier_reached_ = false;
  bool finished_ = false;
  bool failed_ = false;
};

}  // namespace

int main(int argc, char* argv[]) {
  timer_churn::config config;
  switch (timer_churn::parse_options(argc, argv, config)) {
    case timer_churn::parse_result::ok:
      break;
    case timer_churn::parse_result::help:
      timer_churn::print_usage(std::cout, argv[0]);
      return 0;
    case timer_churn::parse_result::error:
      return 2;
  }

  asio::io_context context;
  timer_churn_benchmark benchmark(context, config);
  benchmark.start();

  std::thread runner([&context] { context.run(); });
  runner.join();

  if (!benchmark.successful()) {
    std::cerr << "timer churn benchmark did not complete successfully\n";
    return 1;
  }

  timer_churn::print_results("asio", config, benchmark.metrics(),
                             benchmark.elapsed());
  return 0;
}
