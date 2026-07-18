#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "io_context_runtime_test_support.h"

namespace {

[[nodiscard]] bnio::ip::endpoint bound_loopback_endpoint(
    const bnio::tcp_acceptor& acceptor) {
  sockaddr_in address{};
  socklen_t address_size = sizeof(address);
  EXPECT_TRUE(::getsockname(acceptor.native_handle(),
                            reinterpret_cast<sockaddr*>(&address),
                            &address_size) == 0);
  EXPECT_TRUE(address.sin_family == AF_INET);
  return bnio::ip::endpoint(bnio::ip::address::loopback_v4(),
                            ntohs(address.sin_port));
}

}  // namespace
