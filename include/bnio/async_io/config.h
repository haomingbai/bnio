/**
 * @file config.h
 * @brief Async I/O layer configuration and feature detection.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_CONFIG_H_
#define BNIO_ASYNC_IO_CONFIG_H_

#include <bnio/base/config.h>
#include <bnio/config/system.h>

#if defined(BNIO_SYSTEM_LINUX) && defined(BNIO_HAS_BASE_LINUX) && \
    !defined(BNIO_DISABLE_ASYNC_IO_LINUX)
#define BNIO_HAS_ASYNC_IO_LINUX 1
#endif

#if defined(BNIO_SYSTEM_BSD) && defined(BNIO_HAS_BASE_BSD) && \
    !defined(BNIO_DISABLE_ASYNC_IO_BSD)
#define BNIO_HAS_ASYNC_IO_BSD 1
#endif

#if defined(BNIO_HAS_ASYNC_IO_LINUX) || defined(BNIO_HAS_ASYNC_IO_BSD)
#define BNIO_HAS_ASYNC_IO 1
#endif

#if defined(BNIO_HAS_ASYNC_IO) && \
    !defined(BNIO_DISABLE_ASYNC_IO_IP_ADDRESS_PARSER)
#define BNIO_HAS_ASYNC_IO_IP_ADDRESS_PARSER 1
#endif

#endif  // BNIO_ASYNC_IO_CONFIG_H_
