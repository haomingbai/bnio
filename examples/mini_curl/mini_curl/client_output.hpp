#pragma once
#ifndef BNIO_EXAMPLES_MINI_CURL_CLIENT_OUTPUT_HPP_
#define BNIO_EXAMPLES_MINI_CURL_CLIENT_OUTPUT_HPP_

#include <iostream>
#include <memory>
#include <string_view>

#include "client.hpp"

namespace mini_curl {

inline void mini_curl_client::write_output(std::string_view data) {
  if (options_.output_file.empty()) {
    std::cout.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!std::cout) {
      fail("stdout write failed");
      return;
    }
  } else {
    if (!output_stream_.is_open()) {
      output_stream_.open(options_.output_file,
                          std::ios::binary | std::ios::trunc);
      if (!output_stream_) {
        fail("failed to open output file: " + options_.output_file);
        return;
      }
    }
    output_stream_.write(data.data(),
                         static_cast<std::streamsize>(data.size()));
    if (!output_stream_) {
      fail("write to output file failed");
      return;
    }
  }
}

inline void mini_curl_client::arm_timeout() noexcept {
  if (options_.timeout_seconds <= 0) {
    return;  // timeout disabled
  }

  timer_ = std::make_unique<bnio::steady_timer>(
      context_, std::chrono::seconds(options_.timeout_seconds));

  registry_.spawn(timer_->async_wait(), timer_receiver{shared_from_this()});
}

inline void mini_curl_client::cancel_timeout() noexcept {
  if (timer_) {
    (void)timer_->cancel();
    timer_.reset();
  }
}

inline void mini_curl_client::on_timeout() noexcept {
  if (timed_out_) {
    return;  // already handled
  }
  timed_out_ = true;
  fail("request timed out after " + std::to_string(options_.timeout_seconds) +
       " seconds");
}

inline void mini_curl_client::fail(std::string_view message) noexcept {
  std::cerr << message << '\n';
  finish(1);
}

inline void mini_curl_client::fail(std::string_view message,
                                   std::error_code error) noexcept {
  std::cerr << message << ": " << error.message() << '\n';
  finish(1);
}

inline void mini_curl_client::finish(int code) noexcept {
  if (finished_) return;
  finished_ = true;
  exit_code_ = code;
  cancel_timeout();
  do_stop();
}

// ---- Structured cleanup (final_receiver target) ----

inline void mini_curl_client::do_stop() noexcept {
  ssl_stream_.reset();
  (void)socket_.close();
  if (output_stream_.is_open()) {
    output_stream_.close();
  }
  context_.stop();
}

}  // namespace mini_curl

#endif  // BNIO_EXAMPLES_MINI_CURL_CLIENT_OUTPUT_HPP_
