#pragma once
#ifndef BNIO_EXAMPLES_MINI_CURL_REQUEST_HPP_
#define BNIO_EXAMPLES_MINI_CURL_REQUEST_HPP_

#include <bnio/bnio.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mini_curl {

/**
 * Parsed command-line options for a mini_curl request.
 */
struct request_options {
  /** HTTP method to send. */
  std::string method = "GET";

  /** Remote host name or address. */
  std::string host;

  /** Remote service name or numeric port. */
  std::string service = "80";

  /** Absolute request target sent in the request line. */
  std::string target = "/";

  /** Additional request headers, without trailing CRLF. */
  std::vector<std::string> headers;

  /** Optional request body used by methods such as POST. */
  std::string post_data;

  /** Optional output file path; stdout is used when empty. */
  std::string output_file;

  /** Address family preference used during DNS resolution. */
  bnio::ip::address::version address_version =
      bnio::ip::address::version::unspecified;

  /** Whether to use TLS for the connection. */
  bool use_tls = false;

  /** Whether TLS certificate verification should be disabled. */
  bool insecure = false;

  /** Whether HTTP redirects should be followed. */
  bool follow_redirects = false;

  /** Whether help output was requested. */
  bool help = false;

  /** Whether verbose progress should be written to stderr. */
  bool verbose = false;

  /** Overall request timeout in seconds (0 = no timeout). */
  int timeout_seconds = 30;
};

// ---- URL / header helpers ----

/**
 * Returns true when value begins with prefix.
 */
[[nodiscard]] bool starts_with(std::string_view value,
                               std::string_view prefix) noexcept;

/**
 * Returns true when value contains CR or LF characters.
 */
[[nodiscard]] bool contains_crlf(std::string_view value) noexcept;

/**
 * Normalizes an HTTP request target to an absolute path.
 */
[[nodiscard]] std::string normalized_target(std::string target);

/**
 * Removes a URL fragment from a request target.
 */
[[nodiscard]] std::string remove_fragment(std::string_view target);

/**
 * Parses URL authority text into host and service options.
 */
[[nodiscard]] bool parse_authority(std::string_view authority,
                                   request_options& options,
                                   std::string& error);

/**
 * Parses an HTTP or HTTPS URL into request options.
 */
[[nodiscard]] bool parse_url(std::string_view url, request_options& options,
                             std::string& error);

/**
 * Returns true when a header block already contains the named field.
 */
[[nodiscard]] bool header_contains(std::string_view header,
                                   std::string_view name) noexcept;

/**
 * Builds the value for the Host request header.
 */
[[nodiscard]] std::string host_header_value(const request_options& options);

/**
 * Builds an HTTP/1.1 request from parsed options.
 */
[[nodiscard]] std::string build_request(const request_options& options);

/**
 * Parses the numeric status code from an HTTP status line.
 */
[[nodiscard]] int parse_status_code(std::string_view status_line) noexcept;

/**
 * Extracts the Location header value from a header block.
 */
[[nodiscard]] std::string extract_location(std::string_view headers);

}  // namespace mini_curl

#endif  // BNIO_EXAMPLES_MINI_CURL_REQUEST_HPP_
