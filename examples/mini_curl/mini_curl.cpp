#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "mini_curl_client.hpp"

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
         "      --timeout SECONDS  Overall request timeout (default: 30)\n"
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
      options.address_version = bnio::ip::address::version::v4;
      continue;
    }
    if (arg == "--ipv6") {
      options.address_version = bnio::ip::address::version::v6;
      continue;
    }
    if (arg == "--timeout") {
      std::string value;
      if (!take_value(i, argc, argv, value, error, arg)) {
        std::cerr << error << '\n';
        return false;
      }
      options.timeout_seconds = std::stoi(value);
      continue;
    }
    if (starts_with(arg, "--timeout=")) {
      options.timeout_seconds = std::stoi(std::string(arg.substr(10)));
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

  bnio::io_context context;
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
