#pragma once
#ifndef BUPP_ASYNC_IO_ADDRESS_H_
#define BUPP_ASYNC_IO_ADDRESS_H_

#include <bupp/async_io/config.h>
#include <bupp/async_io/ip/address.h>

namespace bupp::async_io {

/**
 * Alias for the generic IP address type.
 */
using address = ip::address;

#if defined(BUPP_HAS_ASYNC_IO_IP_ADDRESS_PARSER)
/**
 * Imports the generic IP address parser into bupp::async_io.
 */
using ip::make_addr;

/**
 * Imports the generic IP address parser into bupp::async_io.
 */
using ip::make_address;

/**
 * Imports the IPv4 address parser into bupp::async_io.
 */
using ip::make_v4_address;

/**
 * Imports the IPv6 address parser into bupp::async_io.
 */
using ip::make_v6_address;
#endif

}  // namespace bupp::async_io

#endif  // BUPP_ASYNC_IO_ADDRESS_H_
