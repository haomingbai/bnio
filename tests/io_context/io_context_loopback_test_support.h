#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "io_context_runtime_test_support.h"

namespace {

[[nodiscard]] bupp::ip::endpoint bound_loopback_endpoint(
    const bupp::tcp_acceptor& acceptor) {
  sockaddr_in address{};
  socklen_t address_size = sizeof(address);
  assert(::getsockname(acceptor.native_handle(),
                       reinterpret_cast<sockaddr*>(&address),
                       &address_size) == 0);
  assert(address.sin_family == AF_INET);
  return bupp::ip::endpoint(bupp::ip::address::loopback_v4(),
                            ntohs(address.sin_port));
}

}  // namespace
