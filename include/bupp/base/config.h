#pragma once
#ifndef BUPP_BASE_CONFIG_H_
#define BUPP_BASE_CONFIG_H_

#include <bupp/config/system.h>

#if defined(BUPP_SYSTEM_LINUX) && !defined(BUPP_DISABLE_BASE_LINUX)
#define BUPP_HAS_BASE_LINUX 1
#endif

#if defined(BUPP_SYSTEM_BSD) && !defined(BUPP_DISABLE_BASE_BSD)
#define BUPP_HAS_BASE_BSD 1
#endif

#if defined(BUPP_HAS_BASE_LINUX) || defined(BUPP_HAS_BASE_BSD)
#define BUPP_HAS_BASE 1
#endif

#endif  // BUPP_BASE_CONFIG_H_
