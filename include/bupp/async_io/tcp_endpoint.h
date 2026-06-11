#pragma once
#ifndef BUPP_ASYNC_IO_TCP_ENDPOINT_H_
#define BUPP_ASYNC_IO_TCP_ENDPOINT_H_

#include <bupp/async_io/ip/tcp.h>

namespace bupp::async_io {

/**
 * Alias for a TCP endpoint.
 */
using tcp_endpoint = ip::tcp::endpoint;

}  // namespace bupp::async_io

#endif  // BUPP_ASYNC_IO_TCP_ENDPOINT_H_
