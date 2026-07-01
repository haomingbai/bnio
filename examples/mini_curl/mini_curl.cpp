#include <iostream>
#include <string>
#include <string_view>

#include "mini_curl_client.hpp"

namespace mini_curl {

// ============================================================
// URL / header helper implementations
// ============================================================

bool starts_with(std::string_view value, std::string_view prefix) noexcept {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool contains_crlf(std::string_view value) noexcept {
  return value.find('\r') != std::string_view::npos ||
         value.find('\n') != std::string_view::npos;
}

std::string normalized_target(std::string target) {
  if (target.empty()) {
    return "/";
  }
  if (target.front() == '/') {
    return target;
  }
  if (target.front() == '?') {
    return "/" + target;
  }
  return "/" + target;
}

std::string remove_fragment(std::string_view target) {
  const std::size_t fragment = target.find('#');
  if (fragment == std::string_view::npos) {
    return std::string(target);
  }
  return std::string(target.substr(0, fragment));
}

bool parse_authority(std::string_view authority, request_options& options,
                     std::string& error) {
  if (authority.empty()) {
    error = "missing host";
    return false;
  }
  if (authority.find('@') != std::string_view::npos) {
    error = "userinfo in URLs is not supported";
    return false;
  }

  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) {
      error = "missing closing ']' in IPv6 host";
      return false;
    }

    options.host = std::string(authority.substr(1, close - 1));
    const std::string_view rest = authority.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != ':') {
        error = "unexpected characters after IPv6 host";
        return false;
      }
      options.service = std::string(rest.substr(1));
      if (options.service.empty()) {
        error = "empty port";
        return false;
      }
    }
    return true;
  }

  const std::size_t first_colon = authority.find(':');
  const std::size_t last_colon = authority.rfind(':');
  if (first_colon != std::string_view::npos && first_colon == last_colon) {
    options.host = std::string(authority.substr(0, first_colon));
    options.service = std::string(authority.substr(first_colon + 1));
    if (options.host.empty()) {
      error = "missing host";
      return false;
    }
    if (options.service.empty()) {
      error = "empty port";
      return false;
    }
    return true;
  }

  options.host = std::string(authority);
  return true;
}

bool parse_url(std::string_view url, request_options& options,
               std::string& error) {
  const std::size_t scheme_end = url.find("://");
  if (scheme_end != std::string_view::npos) {
    const std::string_view scheme = url.substr(0, scheme_end);
    if (scheme == "https") {
      options.use_tls = true;
      if (options.service == "80") {
        options.service = "443";
      }
    } else if (scheme != "http") {
      error = "unsupported URL scheme: " + std::string(scheme);
      return false;
    }
    url.remove_prefix(scheme_end + 3);
  }

  const std::size_t target_begin = url.find_first_of("/?#");
  const std::string_view authority = target_begin == std::string_view::npos
                                         ? url
                                         : url.substr(0, target_begin);
  if (!parse_authority(authority, options, error)) {
    return false;
  }

  if (target_begin == std::string_view::npos) {
    options.target = "/";
  } else {
    options.target =
        normalized_target(remove_fragment(url.substr(target_begin)));
  }

  return true;
}

bool header_contains(std::string_view header, std::string_view name) noexcept {
  const std::size_t colon = header.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  std::string_view header_name = header.substr(0, colon);
  // Trim trailing whitespace
  while (!header_name.empty() && header_name.back() == ' ') {
    header_name.remove_suffix(1);
  }
  if (header_name.size() != name.size()) {
    return false;
  }
  for (std::size_t i = 0; i < header_name.size(); ++i) {
    char a = header_name[i];
    char b = name[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return true;
}

std::string host_header_value(const request_options& options) {
  std::string host = options.host;
  if (host.find(':') != std::string::npos && host.front() != '[') {
    host = "[" + host + "]";
  }
  const bool is_default_port =
      (options.use_tls &&
       (options.service == "443" || options.service == "https")) ||
      (!options.use_tls &&
       (options.service == "80" || options.service == "http"));
  if (!is_default_port) {
    host += ":" + options.service;
  }
  return host;
}

std::string build_request(const request_options& options) {
  std::string request;
  request += options.method;
  request += " ";
  request += options.target;
  request += " HTTP/1.1\r\nHost: ";
  request += host_header_value(options);
  request += "\r\nUser-Agent: bupp-mini-curl/0.2\r\nAccept: */*\r\n";

  bool has_content_type = false;
  bool has_content_length = false;
  for (const std::string& header : options.headers) {
    request += header;
    request += "\r\n";
    if (header_contains(header, "content-type")) {
      has_content_type = true;
    }
    if (header_contains(header, "content-length")) {
      has_content_length = true;
    }
  }

  if (!options.post_data.empty()) {
    if (!has_content_type) {
      request += "Content-Type: application/x-www-form-urlencoded\r\n";
    }
    if (!has_content_length) {
      request += "Content-Length: ";
      request += std::to_string(options.post_data.size());
      request += "\r\n";
    }
  }

  request += "Connection: close\r\n\r\n";

  if (!options.post_data.empty()) {
    request += options.post_data;
  }

  return request;
}

int parse_status_code(std::string_view status_line) noexcept {
  // Skip "HTTP/x.x "
  const std::size_t first_space = status_line.find(' ');
  if (first_space == std::string_view::npos) {
    return 0;
  }
  std::string_view rest = status_line.substr(first_space + 1);
  // Skip leading spaces
  while (!rest.empty() && rest.front() == ' ') {
    rest.remove_prefix(1);
  }
  const std::size_t second_space = rest.find(' ');
  std::string_view code_str = rest.substr(0, second_space);
  int code = 0;
  for (char c : code_str) {
    if (c < '0' || c > '9') return 0;
    code = code * 10 + (c - '0');
  }
  return code;
}

std::string extract_location(std::string_view headers) {
  // Search for "\nLocation:" or "\r\nLocation:" (case-insensitive)
  std::string lower;
  lower.reserve(headers.size());
  for (char c : headers) {
    lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A'))
                                         : c);
  }

  const std::string needle = "\nlocation:";
  std::size_t pos = lower.find(needle);

  std::size_t value_start;
  if (pos != std::string_view::npos) {
    value_start = pos + 10;  // strlen("\nlocation:")
  } else if (starts_with(lower, "location:")) {
    value_start = 9;  // strlen("location:")
  } else {
    return {};
  }

  // Skip spaces after colon
  while (value_start < headers.size() && headers[value_start] == ' ') {
    ++value_start;
  }

  // Find end of line
  std::size_t value_end = headers.find('\r', value_start);
  if (value_end == std::string_view::npos) {
    value_end = headers.find('\n', value_start);
  }
  if (value_end == std::string_view::npos) {
    value_end = headers.size();
  }

  return std::string(headers.substr(value_start, value_end - value_start));
}

}  // namespace mini_curl

// ============================================================
// main
// ============================================================

namespace {

void print_usage(const char* program) {
  std::cerr
      << "usage: " << program
      << " [options] <url>\n\n"
         "options:\n"
         "  -X, --request METHOD   HTTP method to send (default: GET)\n"
         "  -I, --head             Send a HEAD request\n"
         "  -H, --header HEADER    Add a request header\n"
         "  -d, --data DATA        Send POST data in the request body\n"
         "  -L, --location         Follow redirects (3xx responses)\n"
         "  -o, --output FILE      Write response body to FILE instead of "
         "stdout\n"
         "  -k, --insecure         Skip TLS certificate verification\n"
         "      --host HOST        Override host\n"
         "      --port PORT        Override port/service\n"
         "      --path PATH        Override request path\n"
         "      --ipv4             Resolve only IPv4 addresses\n"
         "      --ipv6             Resolve only IPv6 addresses\n"
         "  -v, --verbose          Print progress to stderr\n";
}

[[nodiscard]] bool take_value(int& index, int argc, char** argv,
                              std::string& value, std::string& error,
                              std::string_view option) {
  if (index + 1 >= argc) {
    error = "missing value for " + std::string(option);
    return false;
  }
  ++index;
  value = argv[index];
  return true;
}

[[nodiscard]] bool parse_args(int argc, char** argv,
                              mini_curl::request_options& options) {
  using mini_curl::contains_crlf;
  using mini_curl::parse_url;
  using mini_curl::starts_with;

  std::string url;
  std::string override_host;
  std::string override_service;
  std::string override_target;
  std::string error;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      options.help = true;
      return true;
    }
    if (arg == "-v" || arg == "--verbose") {
      options.verbose = true;
      continue;
    }
    if (arg == "-I" || arg == "--head") {
      options.method = "HEAD";
      continue;
    }
    if (arg == "-k" || arg == "--insecure") {
      options.insecure = true;
      continue;
    }
    if (arg == "-L" || arg == "--location") {
      options.follow_redirects = true;
      continue;
    }
    if (arg == "--ipv4") {
      options.address_version = bupp::ip::address::version::v4;
      continue;
    }
    if (arg == "--ipv6") {
      options.address_version = bupp::ip::address::version::v6;
      continue;
    }
    if (arg == "-X" || arg == "--request") {
      if (!take_value(i, argc, argv, options.method, error, arg)) {
        std::cerr << error << '\n';
        return false;
      }
      continue;
    }
    if (starts_with(arg, "--request=")) {
      options.method = std::string(arg.substr(10));
      continue;
    }
    if (arg == "-H" || arg == "--header") {
      std::string header;
      if (!take_value(i, argc, argv, header, error, arg)) {
        std::cerr << error << '\n';
        return false;
      }
      if (contains_crlf(header)) {
        std::cerr << "header must not contain CR/LF\n";
        return false;
      }
      options.headers.push_back(std::move(header));
      continue;
    }
    if (starts_with(arg, "--header=")) {
      std::string header(arg.substr(9));
      if (contains_crlf(header)) {
        std::cerr << "header must not contain CR/LF\n";
        return false;
      }
      options.headers.push_back(std::move(header));
      continue;
    }
    if (arg == "-d" || arg == "--data") {
      if (!take_value(i, argc, argv, options.post_data, error, arg)) {
        std::cerr << error << '\n';
        return false;
      }
      continue;
    }
    if (starts_with(arg, "--data=")) {
      options.post_data = std::string(arg.substr(7));
      continue;
    }
    if (arg == "-o" || arg == "--output") {
      if (!take_value(i, argc, argv, options.output_file, error, arg)) {
        std::cerr << error << '\n';
        return false;
      }
      continue;
    }
    if (starts_with(arg, "--output=")) {
      options.output_file = std::string(arg.substr(9));
      continue;
    }
    if (arg == "--host") {
      if (!take_value(i, argc, argv, override_host, error, arg)) {
        std::cerr << error << '\n';
        return false;
      }
      continue;
    }
    if (starts_with(arg, "--host=")) {
      override_host = std::string(arg.substr(7));
      continue;
    }
    if (arg == "--port") {
      if (!take_value(i, argc, argv, override_service, error, arg)) {
        std::cerr << error << '\n';
        return false;
      }
      continue;
    }
    if (starts_with(arg, "--port=")) {
      override_service = std::string(arg.substr(7));
      continue;
    }
    if (arg == "--path") {
      if (!take_value(i, argc, argv, override_target, error, arg)) {
        std::cerr << error << '\n';
        return false;
      }
      continue;
    }
    if (starts_with(arg, "--path=")) {
      override_target = std::string(arg.substr(7));
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      std::cerr << "unknown option: " << arg << '\n';
      return false;
    }
    if (!url.empty()) {
      std::cerr << "only one URL is supported\n";
      return false;
    }
    url = std::string(arg);
  }

  if (!url.empty()) {
    if (!parse_url(url, options, error)) {
      std::cerr << error << '\n';
      return false;
    }
  }
  if (!override_host.empty()) {
    options.host = std::move(override_host);
  }
  if (!override_service.empty()) {
    options.service = std::move(override_service);
  }
  if (!override_target.empty()) {
    options.target = mini_curl::normalized_target(std::move(override_target));
  }

  if (options.host.empty()) {
    print_usage(argv[0]);
    return false;
  }
  if (options.service.empty()) {
    std::cerr << "empty port/service\n";
    return false;
  }
  if (options.method.empty() || contains_crlf(options.method)) {
    std::cerr << "invalid HTTP method\n";
    return false;
  }
  if (contains_crlf(options.target)) {
    std::cerr << "path must not contain CR/LF\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  mini_curl::request_options options;
  if (!parse_args(argc, argv, options)) {
    return 2;
  }
  if (options.help) {
    return 0;
  }

  bupp::io_context context;
  if (!context.is_open()) {
    std::cerr << "io_context unavailable\n";
    return 1;
  }

  mini_curl::operation_registry registry;
  auto client = std::make_shared<mini_curl::mini_curl_client>(
      context, std::move(options), registry);
  client->start();
  context.run();
  const int code = client->exit_code();
  registry.clear();
  return code;
}
