#pragma once
#ifndef BUPP_UDP_H_
#define BUPP_UDP_H_

#include <bupp/async_io/dns/types.h>
#include <bupp/ip.h>
#include <bupp/udp/async_operations.h>  // IWYU pragma: export
#include <bupp/udp/socket.h>            // IWYU pragma: export

#include <cstdint>
#include <string_view>

namespace bupp::udp {

/** Creates a DNS query restricted to UDP endpoints. */
[[nodiscard]] inline dns_query make_resolve_query(
    std::string_view host, std::string_view service,
    ip::udp protocol = {}) noexcept {
  dns_query query(host, service);
  query.set_transport(dns_transport::udp);
  query.set_address_version(protocol.version());
  return query;
}

/** Creates a DNS query restricted to UDP endpoints and a numeric port. */
[[nodiscard]] inline dns_query make_resolve_query(
    std::string_view host, std::uint16_t port, ip::udp protocol = {}) noexcept {
  dns_query query(host, port);
  query.set_transport(dns_transport::udp);
  query.set_address_version(protocol.version());
  return query;
}

}  // namespace bupp::udp

#endif  // BUPP_UDP_H_
