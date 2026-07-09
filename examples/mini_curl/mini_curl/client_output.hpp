#pragma once
#ifndef BUPP_EXAMPLES_MINI_CURL_CLIENT_OUTPUT_HPP_
#define BUPP_EXAMPLES_MINI_CURL_CLIENT_OUTPUT_HPP_

#include <iostream>
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
  exit_code_ = code;
  if (options_.use_tls) {
    // ssl_stream_ holds the socket; destroying it closes the fd
    ssl_stream_.reset();
  }
  (void)socket_.close();
  if (output_stream_.is_open()) {
    output_stream_.close();
  }
  context_.stop();
}

}  // namespace mini_curl

#endif  // BUPP_EXAMPLES_MINI_CURL_CLIENT_OUTPUT_HPP_
