/**
 * @file io_context.h
 * @brief bnio::io_context - the central async runtime.
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_H_
#define BNIO_IO_CONTEXT_H_

#include <bnio/io_context/config.h>

#if defined(BNIO_HAS_IO_CONTEXT_POSIX)
#include <bnio/detail/posix/io_context/class.h>
#else
#error "bnio::io_context requires a POSIX native backend."
#endif

#endif  // BNIO_IO_CONTEXT_H_
