#include <arpa/inet.h>
#include <bnio/async_io/ip/address.h>

#include <string>

namespace bnio::async_io::ip {

std::optional<address> make_address(std::string_view text) {
  if (auto address = make_v4_address(text)) {
    return address;
  }
  return make_v6_address(text);
}

std::optional<address> make_addr(std::string_view text) {
  return make_address(text);
}

std::optional<address> make_v4_address(std::string_view text) {
  const std::string value(text);
  address::v4_bytes parsed{};
  if (::inet_pton(AF_INET, value.c_str(), parsed.data()) != 1) {
    return std::nullopt;
  }
  return address::v4(parsed);
}

std::optional<address> make_v6_address(std::string_view text) {
  const std::string value(text);
  address::v6_bytes parsed{};
  if (::inet_pton(AF_INET6, value.c_str(), parsed.data()) != 1) {
    return std::nullopt;
  }
  return address::v6(parsed);
}

}  // namespace bnio::async_io::ip
