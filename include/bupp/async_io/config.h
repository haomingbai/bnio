#pragma once
#ifndef BUPP_ASYNC_IO_CONFIG_H_
#define BUPP_ASYNC_IO_CONFIG_H_

#include <bupp/base/config.h>
#include <bupp/config/system.h>

#if defined(BUPP_SYSTEM_LINUX) && defined(BUPP_HAS_BASE_LINUX) && \
    !defined(BUPP_DISABLE_ASYNC_IO_LINUX)
#define BUPP_HAS_ASYNC_IO_LINUX 1
#endif

#if defined(BUPP_SYSTEM_BSD) && defined(BUPP_HAS_BASE_BSD) && \
    !defined(BUPP_DISABLE_ASYNC_IO_BSD)
#define BUPP_HAS_ASYNC_IO_BSD 1
#endif

#if defined(BUPP_HAS_ASYNC_IO_LINUX) || defined(BUPP_HAS_ASYNC_IO_BSD)
#define BUPP_HAS_ASYNC_IO 1
#endif

#if defined(BUPP_HAS_ASYNC_IO_LINUX) && \
    !defined(BUPP_DISABLE_ASYNC_IO_IP_ADDRESS_PARSER)
#define BUPP_HAS_ASYNC_IO_IP_ADDRESS_PARSER 1
#endif

#endif  // BUPP_ASYNC_IO_CONFIG_H_
