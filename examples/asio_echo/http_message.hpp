#pragma once
#ifndef BNIO_EXAMPLES_ASIO_ECHO_HTTP_MESSAGE_HPP_
#define BNIO_EXAMPLES_ASIO_ECHO_HTTP_MESSAGE_HPP_

#include <string>
#include <string_view>

namespace asio_echo {

enum class parse_status {
  incomplete,
  ready,
  bad_request,
};

struct parsed_request {
  std::string method;
  std::string target;
  std::string body;
  bool keep_alive = true;
};

[[nodiscard]] parse_status try_parse_request(std::string_view bytes,
                                             parsed_request& request);

[[nodiscard]] std::string make_response(const parsed_request& request);

[[nodiscard]] std::string make_bad_request_response();

}  // namespace asio_echo

#endif  // BNIO_EXAMPLES_ASIO_ECHO_HTTP_MESSAGE_HPP_
