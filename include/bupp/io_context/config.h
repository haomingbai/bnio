#pragma once
#ifndef BUPP_IO_CONTEXT_CONFIG_H_
#define BUPP_IO_CONTEXT_CONFIG_H_

#include <bupp/async_io/config.h>
#include <bupp/config/system.h>

#if defined(BUPP_SYSTEM_LINUX) && defined(BUPP_HAS_ASYNC_IO_LINUX) && \
    !defined(BUPP_DISABLE_IO_CONTEXT_LINUX)
#define BUPP_HAS_IO_CONTEXT_LINUX 1
#endif

#if defined(BUPP_SYSTEM_BSD) && defined(BUPP_HAS_ASYNC_IO_BSD) && \
    !defined(BUPP_DISABLE_IO_CONTEXT_BSD)
#define BUPP_HAS_IO_CONTEXT_BSD 1
#endif

#if defined(BUPP_HAS_IO_CONTEXT_LINUX) || defined(BUPP_HAS_IO_CONTEXT_BSD)
#define BUPP_HAS_IO_CONTEXT 1
#endif

#endif  // BUPP_IO_CONTEXT_CONFIG_H_
