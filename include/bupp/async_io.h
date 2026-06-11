#pragma once
#ifndef BUPP_ASYNC_IO_H_
#define BUPP_ASYNC_IO_H_

#include <bupp/async_io/address.h>
#include <bupp/async_io/buffer_view.h>
#include <bupp/async_io/descriptor_view.h>
#include <bupp/async_io/ip/address.h>
#include <bupp/async_io/ip/endpoint.h>
#include <bupp/async_io/ip/tcp.h>
#include <bupp/async_io/ip/udp.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/async_io/time.h>

/**
 * @file
 * Umbrella header for the platform-neutral async I/O vocabulary types.
 *
 * This layer intentionally contains only value types and non-owning views. It
 * does not expose sender factories, RAII socket or stream owners, or SSL
 * context objects; those belong to the higher-level bupp::io_context,
 * bupp::ip::tcp, and bupp::ssl APIs.
 */

#endif  // BUPP_ASYNC_IO_H_
