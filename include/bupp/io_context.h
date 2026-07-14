#pragma once
#ifndef BUPP_IO_CONTEXT_H_
#define BUPP_IO_CONTEXT_H_

#include <bupp/io_context/config.h>

#if defined(BUPP_HAS_IO_CONTEXT_LINUX)
#include <bupp/linux/io_context.h>
#elif defined(BUPP_HAS_IO_CONTEXT_BSD)
#include <bupp/bsd/io_context.h>
#else
#error "bupp::io_context requires a supported native backend."
#endif

#endif  // BUPP_IO_CONTEXT_H_
