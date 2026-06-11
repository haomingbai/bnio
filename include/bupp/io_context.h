#pragma once
#ifndef BUPP_IO_CONTEXT_H_
#define BUPP_IO_CONTEXT_H_

#if defined(__linux__)
#include <bupp/linux/io_context.h>
#else
#error "bupp::io_context is currently implemented only on Linux."
#endif

#endif  // BUPP_IO_CONTEXT_H_
