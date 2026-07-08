#pragma once
#ifndef BUPP_IO_CONTEXT_H_
#define BUPP_IO_CONTEXT_H_

#include <bupp/io_context/config.h>

// io_context is implemented only when the Linux io_uring backend is enabled.
#if defined(BUPP_HAS_IO_CONTEXT_LINUX)
#include <bupp/linux/io_context.h>
#else
#error "bupp::io_context requires BUPP_HAS_IO_CONTEXT_LINUX."
#endif

#endif  // BUPP_IO_CONTEXT_H_
