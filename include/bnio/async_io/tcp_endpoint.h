/**
 * @file tcp_endpoint.h
 * @brief TCP endpoint combining IP address and port.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_TCP_ENDPOINT_H_
#define BNIO_ASYNC_IO_TCP_ENDPOINT_H_

#include <bnio/async_io/ip/tcp.h>

namespace bnio::async_io {

/**
 * Alias for a TCP endpoint.
 */
using tcp_endpoint = ip::tcp::endpoint;

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_TCP_ENDPOINT_H_
