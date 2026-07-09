#include "http_message.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace asio_echo {
namespace {

constexpr std::size_t k_max_request_size = 1024U * 1024U;

[[nodiscard]] bool is_space(char ch) noexcept {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

[[nodiscard]] char lower_ascii(char ch) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && is_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_space(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] bool iequals(std::string_view lhs,
                           std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (lower_ascii(lhs[index]) != lower_ascii(rhs[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool token_contains(std::string_view value,
                                  std::string_view token) noexcept {
  while (!value.empty()) {
    const std::size_t comma = value.find(',');
    const std::string_view part = trim(value.substr(0, comma));
    if (iequals(part, token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    value.remove_prefix(comma + 1);
  }
  return false;
}

}  // namespace

[[nodiscard]] parse_status try_parse_request(std::string_view bytes,
                                             parsed_request& request) {
  const std::size_t header_end = bytes.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return bytes.size() > k_max_request_size ? parse_status::bad_request
                                             : parse_status::incomplete;
  }

  const std::size_t first_line_end = bytes.find("\r\n");
  if (first_line_end == std::string_view::npos || first_line_end > header_end) {
    return parse_status::bad_request;
  }

  std::string_view first_line = bytes.substr(0, first_line_end);
  const std::size_t method_end = first_line.find(' ');
  if (method_end == std::string_view::npos) {
    return parse_status::bad_request;
  }
  first_line.remove_prefix(method_end + 1);
  const std::size_t target_end = first_line.find(' ');
  if (target_end == std::string_view::npos) {
    return parse_status::bad_request;
  }

  const std::string_view method = bytes.substr(0, method_end);
  const std::string_view target = first_line.substr(0, target_end);
  const std::string_view version = first_line.substr(target_end + 1);

  std::size_t content_length = 0;
  bool saw_connection_close = false;
  bool saw_connection_keep_alive = false;

  std::size_t line_begin = first_line_end + 2;
  while (line_begin < header_end) {
    const std::size_t line_end = bytes.find("\r\n", line_begin);
    if (line_end == std::string_view::npos || line_end > header_end) {
      return parse_status::bad_request;
    }

    const std::string_view line =
        bytes.substr(line_begin, line_end - line_begin);
    line_begin = line_end + 2;
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }

    const std::string_view name = trim(line.substr(0, colon));
    const std::string_view value = trim(line.substr(colon + 1));
    if (iequals(name, "content-length")) {
      const char* first = value.data();
      const char* last = value.data() + value.size();
      const auto [ptr, ec] = std::from_chars(first, last, content_length);
      if (ec != std::errc{} || ptr != last) {
        return parse_status::bad_request;
      }
    } else if (iequals(name, "connection")) {
      saw_connection_close = token_contains(value, "close");
      saw_connection_keep_alive = token_contains(value, "keep-alive");
    }
  }

  const std::size_t body_begin = header_end + 4;
  if (content_length > k_max_request_size ||
      body_begin + content_length > k_max_request_size) {
    return parse_status::bad_request;
  }
  if (bytes.size() < body_begin + content_length) {
    return parse_status::incomplete;
  }

  request.method.assign(method);
  request.target.assign(target);
  request.body.assign(bytes.substr(body_begin, content_length));
  request.keep_alive =
      !saw_connection_close &&
      (!iequals(version, "HTTP/1.0") || saw_connection_keep_alive);
  return parse_status::ready;
}

[[nodiscard]] std::string make_response(const parsed_request& request) {
  std::string body = request.body;
  if (body.empty()) {
    body = request.method + " " + request.target + "\n";
  }

  std::string response;
  response.reserve(128 + body.size());
  response += "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: text/plain\r\n";
  response += "Content-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: ";
  response += request.keep_alive ? "keep-alive" : "close";
  response += "\r\n\r\n";
  response += body;
  return response;
}

[[nodiscard]] std::string make_bad_request_response() {
  constexpr std::string_view body = "bad request\n";
  std::string response;
  response.reserve(128 + body.size());
  response += "HTTP/1.1 400 Bad Request\r\n";
  response += "Content-Type: text/plain\r\n";
  response += "Content-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: close\r\n\r\n";
  response += body;
  return response;
}

}  // namespace asio_echo
