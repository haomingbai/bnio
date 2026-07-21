/**
 * @file bnio.h
 * @brief Aggregate header for the entire bnio library.
 */

#pragma once
#ifndef BNIO_BNIO_H_
#define BNIO_BNIO_H_

#include <bnio/async_io.h>
#include <bnio/base.h>
#include <bnio/buffer.h>
#include <bnio/export.h>
#include <bnio/io_context/config.h>
#include <bnio/ip.h>

#if defined(BNIO_HAS_IO_CONTEXT)
#include <bnio/io_context.h>
#include <bnio/ssl.h>
#include <bnio/tcp.h>
#include <bnio/udp.h>
#endif

#endif  // BNIO_BNIO_H_
