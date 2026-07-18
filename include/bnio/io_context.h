#pragma once
#ifndef BNIO_IO_CONTEXT_H_
#define BNIO_IO_CONTEXT_H_

#include <bnio/io_context/config.h>

#if defined(BNIO_HAS_IO_CONTEXT_LINUX)
#include <bnio/linux/io_context.h>
#elif defined(BNIO_HAS_IO_CONTEXT_BSD)
#include <bnio/bsd/io_context.h>
#else
#error "bnio::io_context requires a supported native backend."
#endif

#endif  // BNIO_IO_CONTEXT_H_
