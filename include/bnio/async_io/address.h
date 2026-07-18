#pragma once
#ifndef BNIO_ASYNC_IO_ADDRESS_H_
#define BNIO_ASYNC_IO_ADDRESS_H_

#include <bnio/async_io/config.h>
#include <bnio/async_io/ip/address.h>

namespace bnio::async_io {

/**
 * Alias for the generic IP address type.
 */
using address = ip::address;

#if defined(BNIO_HAS_ASYNC_IO_IP_ADDRESS_PARSER)
/**
 * Imports the generic IP address parser into bnio::async_io.
 */
using ip::make_addr;

/**
 * Imports the generic IP address parser into bnio::async_io.
 */
using ip::make_address;

/**
 * Imports the IPv4 address parser into bnio::async_io.
 */
using ip::make_v4_address;

/**
 * Imports the IPv6 address parser into bnio::async_io.
 */
using ip::make_v6_address;
#endif

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_ADDRESS_H_
