#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string_view>
#include <system_error>

namespace timer_churn {

struct config {
  std::size_t timer_count = 1024;
  std::size_t update_rounds = 100;
  std::size_t replacements_per_round = 0;
};

struct metrics {
  std::size_t timers_created = 0;
  std::size_t timers_destroyed = 0;
  std::size_t expiry_sets = 0;
  std::size_t explicit_cancels = 0;
  std::size_t waits_started = 0;
  std::size_t waits_stopped = 0;
};

enum class parse_result {
  ok,
  help,
  error,
};

inline void print_usage(std::ostream& stream, std::string_view program) {
  stream << "usage: " << program << " [options]\n\n"
         << "Runs an active timer-churn workload. Every update round destroys "
            "and\n"
         << "recreates a rotating subset, resets the other timers' expiry, "
            "and\n"
         << "starts a new wait for every live timer.\n\n"
         << "options:\n"
         << "  --timers N          Live timers in every round (default: 1024)\n"
         << "  --updates N         Reset/replacement rounds (default: 100)\n"
         << "  --replace-count N   Timers destroyed and recreated per round\n"
         << "                      (default: one quarter of --timers)\n"
         << "  --help, -h          Show this message\n";
}

[[nodiscard]] inline bool parse_positive_size(std::string_view text,
                                              std::size_t& value) {
  if (text.empty()) {
    return false;
  }

  std::size_t parsed = 0;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto [position, error] = std::from_chars(first, last, parsed);
  if (error != std::errc{} || position != last || parsed == 0) {
    return false;
  }

  value = parsed;
  return true;
}

[[nodiscard]] inline bool parse_option_value(int& index, int argc, char* argv[],
                                             std::size_t& value,
                                             std::string_view option) {
  if (++index >= argc ||
      !parse_positive_size(std::string_view{argv[index]}, value)) {
    std::cerr << option << " requires a positive integer\n";
    return false;
  }
  return true;
}

[[nodiscard]] inline parse_result parse_options(int argc, char* argv[],
                                                config& result) {
  bool replacement_count_supplied = false;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help" || argument == "-h") {
      return parse_result::help;
    }

    if (argument == "--timers") {
      if (!parse_option_value(index, argc, argv, result.timer_count,
                              "--timers")) {
        return parse_result::error;
      }
      continue;
    }
    if (argument == "--updates") {
      if (!parse_option_value(index, argc, argv, result.update_rounds,
                              "--updates")) {
        return parse_result::error;
      }
      continue;
    }
    if (argument == "--replace-count") {
      if (!parse_option_value(index, argc, argv, result.replacements_per_round,
                              "--replace-count")) {
        return parse_result::error;
      }
      replacement_count_supplied = true;
      continue;
    }

    constexpr std::string_view timers_prefix = "--timers=";
    constexpr std::string_view updates_prefix = "--updates=";
    constexpr std::string_view replace_prefix = "--replace-count=";
    if (argument.starts_with(timers_prefix)) {
      if (!parse_positive_size(argument.substr(timers_prefix.size()),
                               result.timer_count)) {
        std::cerr << "--timers requires a positive integer\n";
        return parse_result::error;
      }
      continue;
    }
    if (argument.starts_with(updates_prefix)) {
      if (!parse_positive_size(argument.substr(updates_prefix.size()),
                               result.update_rounds)) {
        std::cerr << "--updates requires a positive integer\n";
        return parse_result::error;
      }
      continue;
    }
    if (argument.starts_with(replace_prefix)) {
      if (!parse_positive_size(argument.substr(replace_prefix.size()),
                               result.replacements_per_round)) {
        std::cerr << "--replace-count requires a positive integer\n";
        return parse_result::error;
      }
      replacement_count_supplied = true;
      continue;
    }

    std::cerr << "unknown option: " << argument << '\n';
    return parse_result::error;
  }

  if (result.timer_count < 2) {
    std::cerr << "--timers must be at least 2\n";
    return parse_result::error;
  }
  if (!replacement_count_supplied) {
    result.replacements_per_round =
        std::max<std::size_t>(1, result.timer_count / 4);
  }
  if (result.replacements_per_round >= result.timer_count) {
    std::cerr << "--replace-count must be smaller than --timers so the "
                 "benchmark also resets expiry on live timers\n";
    return parse_result::error;
  }

  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (result.update_rounds == maximum ||
      result.timer_count > maximum / (result.update_rounds + 1)) {
    std::cerr << "timer count multiplied by update rounds overflows\n";
    return parse_result::error;
  }

  using milliseconds_rep = std::chrono::milliseconds::rep;
  if (result.update_rounds >
      static_cast<std::size_t>(std::numeric_limits<milliseconds_rep>::max())) {
    std::cerr << "--updates is too large\n";
    return parse_result::error;
  }
  using nanoseconds_rep = std::chrono::nanoseconds::rep;
  if (result.timer_count >
      static_cast<std::size_t>(std::numeric_limits<nanoseconds_rep>::max())) {
    std::cerr << "--timers is too large\n";
    return parse_result::error;
  }

  return parse_result::ok;
}

inline void print_results(std::string_view backend, const config& config,
                          const metrics& metrics,
                          std::chrono::steady_clock::duration elapsed) {
  const double seconds = std::chrono::duration<double>(elapsed).count();
  const double lifecycle_calls_per_second =
      seconds > 0.0 ? static_cast<double>(
                          metrics.timers_created + metrics.timers_destroyed +
                          metrics.expiry_sets + metrics.explicit_cancels) /
                          seconds
                    : 0.0;
  const double active_waits_per_second =
      seconds > 0.0 ? static_cast<double>(metrics.waits_started) / seconds
                    : 0.0;

  std::cout << "timer churn benchmark\n"
            << "  backend: " << backend << '\n'
            << "  live timers: " << config.timer_count << '\n'
            << "  update rounds: " << config.update_rounds << '\n'
            << "  replacements per round: " << config.replacements_per_round
            << '\n'
            << "  elapsed: " << seconds << " s\n"
            << "  timer creates: " << metrics.timers_created << '\n'
            << "  timer destroys: " << metrics.timers_destroyed << '\n'
            << "  expiry sets: " << metrics.expiry_sets << '\n'
            << "  explicit cancels: " << metrics.explicit_cancels << '\n'
            << "  active waits started/stopped: " << metrics.waits_started
            << '/' << metrics.waits_stopped << '\n'
            << "  lifecycle API calls/s: " << lifecycle_calls_per_second << '\n'
            << "  active waits/s: " << active_waits_per_second << '\n';
}

}  // namespace timer_churn
