#pragma once
#ifndef BNIO_ASYNC_IO_H_
#define BNIO_ASYNC_IO_H_

#include <bnio/async_io/address.h>
#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/config.h>
#include <bnio/async_io/descriptor_view.h>
#include <bnio/async_io/dns.h>
#include <bnio/async_io/ip/address.h>
#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/ip/tcp.h>
#include <bnio/async_io/ip/udp.h>
#include <bnio/async_io/socket_view.h>
#include <bnio/async_io/time.h>

/**
 * @file
 * Umbrella header for the platform-neutral async I/O vocabulary types.
 *
 * This layer intentionally contains only value types and non-owning views. It
 * does not expose sender factories, RAII socket or stream owners, or SSL
 * context objects; those belong to the higher-level bnio::io_context,
 * bnio::ip::{tcp,udp}, and bnio::ssl APIs.
 */

#endif  // BNIO_ASYNC_IO_H_
