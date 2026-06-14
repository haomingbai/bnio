#include <bupp/bupp.h>
#include <sys/socket.h>

#include <array>
#include <bexec/bexec.hpp>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t k_max_endpoints = 16;
constexpr std::size_t k_receive_size = 16 * 1024;

struct operation_holder_base {
  virtual ~operation_holder_base() = default;
  virtual void start() noexcept = 0;
};

std::vector<std::unique_ptr<operation_holder_base>> g_ops;

template <class Sender, class Receiver>
void spawn(Sender&& sender, Receiver&& receiver) {
  using operation_type = decltype(bexec::connect(std::declval<Sender>(),
                                                 std::declval<Receiver>()));

  struct holder final : operation_holder_base {
    operation_type operation;

    holder(Sender&& s, Receiver&& r)
        : operation(bexec::connect(std::forward<Sender>(s),
                                   std::forward<Receiver>(r))) {}

    void start() noexcept override { bexec::start(operation); }
  };

  auto op = std::make_unique<holder>(std::forward<Sender>(sender),
                                     std::forward<Receiver>(receiver));
  op->start();
  g_ops.push_back(std::move(op));
}

struct request_options {
  std::string method = "GET";
  std::string host;
  std::string service = "80";
  std::string target = "/";
  std::vector<std::string> headers;
  bupp::ip::address::version address_version =
      bupp::ip::address::version::unspecified;
  bool help = false;
  bool verbose = false;
};

void print_usage(const char* program) {
  std::cerr << "usage: " << program
            << " [options] http://host[:port]/path\n\n"
               "options:\n"
               "  -X, --request METHOD   HTTP method to send (default: GET)\n"
               "  -I, --head             Send a HEAD request\n"
               "  -H, --header HEADER    Add a request header\n"
               "      --host HOST        Override host\n"
               "      --port PORT        Override port/service\n"
               "      --path PATH        Override request path\n"
               "      --ipv4             Resolve only IPv4 addresses\n"
               "      --ipv6             Resolve only IPv6 addresses\n"
               "  -v, --verbose          Print progress to stderr\n";
}

[[nodiscard]] bool starts_with(std::string_view value,
                               std::string_view prefix) noexcept {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool contains_crlf(std::string_view value) noexcept {
  return value.find('\r') != std::string_view::npos ||
         value.find('\n') != std::string_view::npos;
}

[[nodiscard]] std::string normalized_target(std::string target) {
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

[[nodiscard]] std::string remove_fragment(std::string_view target) {
  const std::size_t fragment = target.find('#');
  if (fragment == std::string_view::npos) {
    return std::string(target);
  }
  return std::string(target.substr(0, fragment));
}

[[nodiscard]] bool parse_authority(std::string_view authority,
                                   request_options& options,
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

[[nodiscard]] bool parse_url(std::string_view url, request_options& options,
                             std::string& error) {
  const std::size_t scheme_end = url.find("://");
  if (scheme_end != std::string_view::npos) {
    const std::string_view scheme = url.substr(0, scheme_end);
    if (scheme != "http") {
      if (scheme == "https") {
        error = "https:// is not supported by this tiny example yet";
      } else {
        error = "unsupported URL scheme: " + std::string(scheme);
      }
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

[[nodiscard]] bool parse_args(int argc, char** argv, request_options& options) {
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
    options.target = normalized_target(std::move(override_target));
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

[[nodiscard]] std::string host_header_value(const request_options& options) {
  std::string host = options.host;
  if (host.find(':') != std::string::npos && host.front() != '[') {
    host = "[" + host + "]";
  }
  if (options.service != "80" && options.service != "http") {
    host += ":" + options.service;
  }
  return host;
}

[[nodiscard]] std::string build_request(const request_options& options) {
  std::string request;
  request += options.method;
  request += " ";
  request += options.target;
  request += " HTTP/1.1\r\nHost: ";
  request += host_header_value(options);
  request += "\r\nUser-Agent: bupp-mini-curl/0.1\r\nAccept: */*\r\n";
  for (const std::string& header : options.headers) {
    request += header;
    request += "\r\n";
  }
  request += "Connection: close\r\n\r\n";
  return request;
}

class mini_curl_client : public std::enable_shared_from_this<mini_curl_client> {
 public:
  mini_curl_client(bupp::io_context& context, request_options options)
      : context_(context), options_(std::move(options)) {}

  void start() { resolve(); }

  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

 private:
  struct resolve_receiver {
    std::shared_ptr<mini_curl_client> client;

    void set_value(std::size_t count) noexcept { client->on_resolved(count); }
    void set_error(std::error_code error) noexcept {
      client->fail("resolve failed", error);
    }
    void set_stopped() noexcept { client->fail("resolve stopped"); }
  };

  struct connect_receiver {
    std::shared_ptr<mini_curl_client> client;

    void set_value() noexcept { client->on_connected(); }
    void set_error(std::error_code error) noexcept {
      client->on_connect_error(error);
    }
    void set_stopped() noexcept { client->fail("connect stopped"); }
  };

  struct write_receiver {
    std::shared_ptr<mini_curl_client> client;

    void set_value(std::size_t) noexcept { client->receive(); }
    void set_error(std::error_code error) noexcept {
      client->fail("write failed", error);
    }
    void set_stopped() noexcept { client->fail("write stopped"); }
  };

  struct receive_receiver {
    std::shared_ptr<mini_curl_client> client;

    void set_value(std::size_t count) noexcept { client->on_received(count); }
    void set_error(std::error_code error) noexcept {
      client->fail("receive failed", error);
    }
    void set_stopped() noexcept { client->fail("receive stopped"); }
  };

  void resolve() {
    if (options_.verbose) {
      std::cerr << "* Resolving " << options_.host << ":" << options_.service
                << '\n';
    }

    bupp::dns_query query(options_.host, options_.service);
    query.set_address_version(options_.address_version);
    query.set_transport(bupp::dns_transport::tcp);

    const auto scheduler = context_.get_post_scheduler();
    spawn(scheduler.async_resolve(
              std::move(query),
              bupp::dns_result_view(endpoints_.data(), endpoints_.size())),
          resolve_receiver{shared_from_this()});
  }

  void on_resolved(std::size_t count) noexcept {
    endpoint_count_ = count;
    endpoint_index_ = 0;
    if (endpoint_count_ == 0) {
      fail("resolve returned no endpoints");
      return;
    }
    if (options_.verbose) {
      std::cerr << "* Resolved " << endpoint_count_ << " endpoint(s)\n";
    }
    connect_next();
  }

  void connect_next() noexcept {
    if (endpoint_index_ >= endpoint_count_) {
      if (last_connect_error_) {
        fail("connect failed", last_connect_error_);
      } else {
        fail("connect failed");
      }
      return;
    }

    (void)socket_.close();
    const bupp::ip::endpoint endpoint = endpoints_[endpoint_index_];
    ++endpoint_index_;

    const bupp::ip::address::version version = endpoint.version();
    std::error_code open_error;
    if (version == bupp::ip::address::version::v4) {
      open_error = socket_.open(bupp::ip::tcp::v4());
    } else if (version == bupp::ip::address::version::v6) {
      open_error = socket_.open(bupp::ip::tcp::v6());
    } else {
      connect_next();
      return;
    }

    if (open_error) {
      last_connect_error_ = open_error;
      connect_next();
      return;
    }

    if (options_.verbose) {
      std::cerr << "* Connecting to endpoint " << endpoint_index_ << "/"
                << endpoint_count_ << '\n';
    }

    const auto scheduler = context_.get_post_scheduler();
    spawn(scheduler.async_connect(socket_, endpoint),
          connect_receiver{shared_from_this()});
  }

  void on_connect_error(std::error_code error) noexcept {
    last_connect_error_ = error;
    connect_next();
  }

  void on_connected() noexcept {
    if (options_.verbose) {
      std::cerr << "* Connected; sending request\n";
    }

    request_ = build_request(options_);
    const auto scheduler = context_.get_post_scheduler();
    spawn(scheduler.async_write(socket_, bupp::buffer(request_), MSG_NOSIGNAL),
          write_receiver{shared_from_this()});
  }

  void receive() noexcept {
    const auto scheduler = context_.get_post_scheduler();
    spawn(scheduler.async_receive(socket_, bupp::buffer(receive_buffer_)),
          receive_receiver{shared_from_this()});
  }

  void on_received(std::size_t count) noexcept {
    if (count == 0) {
      finish(0);
      return;
    }

    std::cout.write(receive_buffer_.data(),
                    static_cast<std::streamsize>(count));
    if (!std::cout) {
      fail("stdout write failed");
      return;
    }
    receive();
  }

  void fail(std::string_view message) noexcept {
    std::cerr << message << '\n';
    finish(1);
  }

  void fail(std::string_view message, std::error_code error) noexcept {
    std::cerr << message << ": " << error.message() << '\n';
    finish(1);
  }

  void finish(int code) noexcept {
    exit_code_ = code;
    (void)socket_.close();
    (void)context_.stop();
  }

  bupp::io_context& context_;
  request_options options_;
  bupp::tcp_socket socket_;
  std::array<bupp::ip::endpoint, k_max_endpoints> endpoints_{};
  std::array<char, k_receive_size> receive_buffer_{};
  std::size_t endpoint_count_ = 0;
  std::size_t endpoint_index_ = 0;
  std::string request_;
  std::error_code last_connect_error_;
  int exit_code_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  request_options options;
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

  auto client = std::make_shared<mini_curl_client>(context, std::move(options));
  client->start();
  context.run();
  const int code = client->exit_code();
  g_ops.clear();
  return code;
}
