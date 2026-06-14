#include <bupp/async_io/dns.h>
#include <bupp/async_io/linux/socket_address.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <string>

namespace bupp::async_io {
namespace {

class dns_error_category final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override {
    return "bupp.dns";
  }

  [[nodiscard]] std::string message(int condition) const override {
    const char* message = ::gai_strerror(condition);
    return message == nullptr ? "unknown DNS resolver error"
                              : std::string(message);
  }
};

[[nodiscard]] int native_family(ip::address::version version) noexcept {
  switch (version) {
    case ip::address::version::v4:
      return AF_INET;
    case ip::address::version::v6:
      return AF_INET6;
    case ip::address::version::unspecified:
      return AF_UNSPEC;
  }
  return AF_UNSPEC;
}

[[nodiscard]] int native_socket_type(dns_transport transport) noexcept {
  switch (transport) {
    case dns_transport::tcp:
      return SOCK_STREAM;
    case dns_transport::udp:
      return SOCK_DGRAM;
    case dns_transport::any:
      return 0;
  }
  return 0;
}

[[nodiscard]] int native_protocol(dns_transport transport) noexcept {
  switch (transport) {
    case dns_transport::tcp:
      return IPPROTO_TCP;
    case dns_transport::udp:
      return IPPROTO_UDP;
    case dns_transport::any:
      return 0;
  }
  return 0;
}

[[nodiscard]] int native_flags(dns_query_flags flags) noexcept {
  int native = 0;
  if (has_dns_query_flag(flags, dns_query_flags::passive)) {
    native |= AI_PASSIVE;
  }
  if (has_dns_query_flag(flags, dns_query_flags::canonical_name)) {
    native |= AI_CANONNAME;
  }
  if (has_dns_query_flag(flags, dns_query_flags::numeric_host)) {
    native |= AI_NUMERICHOST;
  }
  if (has_dns_query_flag(flags, dns_query_flags::numeric_service)) {
    native |= AI_NUMERICSERV;
  }
  return native;
}

[[nodiscard]] std::error_code make_resolve_error(int result,
                                                 int system_errno) noexcept {
  if (result == EAI_SYSTEM && system_errno != 0) {
    return std::error_code(system_errno, std::generic_category());
  }
  return make_dns_error_code(result);
}

}  // namespace

const std::error_category& dns_category() noexcept {
  static const dns_error_category category;
  return category;
}

std::error_code make_dns_error_code(int result) noexcept {
  return std::error_code(result, dns_category());
}

namespace detail {

std::error_code resolve_dns_platform(dns_query_view query,
                                     dns_result_view output,
                                     std::size_t& count) noexcept {
  if (!query.valid) {
    return std::make_error_code(std::errc::invalid_argument);
  }

  addrinfo hints{};
  hints.ai_family = native_family(query.address_version);
  hints.ai_socktype = native_socket_type(query.transport);
  hints.ai_protocol = native_protocol(query.transport);
  hints.ai_flags = native_flags(query.flags);

  addrinfo* results = nullptr;
  errno = 0;
  const int result = ::getaddrinfo(query.host, query.service, &hints, &results);
  const int system_errno = errno;

  if (result != 0) {
    return make_resolve_error(result, system_errno);
  }

  for (const addrinfo* current = results; current != nullptr;
       current = current->ai_next) {
    auto endpoint = linux_native::make_endpoint(
        current->ai_addr, static_cast<socklen_t>(current->ai_addrlen));
    if (endpoint.has_value()) {
      if (count >= output.size()) {
        break;
      }
      output[count] = *endpoint;
      ++count;
    }
  }

  ::freeaddrinfo(results);
  return {};
}

}  // namespace detail

}  // namespace bupp::async_io
