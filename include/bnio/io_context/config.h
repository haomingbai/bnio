/**
 * @file config.h
 * @brief io_context layer configuration and feature detection.
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_CONFIG_H_
#define BNIO_IO_CONTEXT_CONFIG_H_

#include <bnio/async_io/config.h>
#include <bnio/config/system.h>

#if defined(BNIO_SYSTEM_LINUX) && defined(BNIO_HAS_ASYNC_IO_LINUX) && \
    !defined(BNIO_DISABLE_IO_CONTEXT_LINUX)
#define BNIO_HAS_IO_CONTEXT_LINUX 1
#endif

#if defined(BNIO_SYSTEM_BSD) && defined(BNIO_HAS_ASYNC_IO_BSD) && \
    !defined(BNIO_DISABLE_IO_CONTEXT_BSD)
#define BNIO_HAS_IO_CONTEXT_BSD 1
#endif

#if defined(BNIO_HAS_IO_CONTEXT_LINUX) || defined(BNIO_HAS_IO_CONTEXT_BSD)
#define BNIO_HAS_IO_CONTEXT 1
#endif

#endif  // BNIO_IO_CONTEXT_CONFIG_H_
