#pragma once
#ifndef BUPP_EXAMPLES_MINI_CURL_CLIENT_REDIRECT_HPP_
#define BUPP_EXAMPLES_MINI_CURL_CLIENT_REDIRECT_HPP_

#include <iostream>
#include <utility>

#include "client.hpp"

namespace mini_curl {

inline void mini_curl_client::follow_redirect(std::string location) {
  ++redirect_count_;

  // Close current connection cleanly
  if (options_.use_tls) {
    // ssl_stream_ owns the moved-from socket_; destroying it closes the fd
    ssl_stream_.reset();
  }
  // socket_ may be moved-from; close() is a no-op on fd == -1
  (void)socket_.close();

  // Parse the redirect URL
  request_options new_options;
  new_options.method = "GET";  // RFC 7231: change to GET on redirect
  new_options.service = options_.use_tls ? "443" : "80";
  new_options.use_tls = options_.use_tls;

  std::string error;
  if (!parse_url(location, new_options, error)) {
    // Not an absolute URL — treat as relative path
    new_options = options_;
    new_options.method = "GET";
    new_options.post_data.clear();
    new_options.target = normalized_target(location);
  }

  // Carry forward user-specified options
  new_options.headers = options_.headers;
  new_options.verbose = options_.verbose;
  new_options.insecure = options_.insecure;
  new_options.follow_redirects = options_.follow_redirects;
  new_options.output_file = options_.output_file;
  new_options.address_version = options_.address_version;

  options_ = std::move(new_options);

  endpoint_index_ = 0;
  endpoint_count_ = 0;
  last_connect_error_ = std::error_code{};

  if (options_.verbose) {
    std::cerr << "* Redirect #" << redirect_count_ << " to "
              << (options_.use_tls ? "https://" : "http://") << options_.host
              << ":" << options_.service << options_.target << '\n';
  }

  resolve();
}

}  // namespace mini_curl

#endif  // BUPP_EXAMPLES_MINI_CURL_CLIENT_REDIRECT_HPP_
