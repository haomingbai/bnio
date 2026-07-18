#pragma once
#ifndef BNIO_BASE_CONFIG_H_
#define BNIO_BASE_CONFIG_H_

#include <bnio/config/system.h>

#if defined(BNIO_SYSTEM_LINUX) && !defined(BNIO_DISABLE_BASE_LINUX)
#define BNIO_HAS_BASE_LINUX 1
#endif

#if defined(BNIO_SYSTEM_BSD) && !defined(BNIO_DISABLE_BASE_BSD)
#define BNIO_HAS_BASE_BSD 1
#endif

#if defined(BNIO_HAS_BASE_LINUX) || defined(BNIO_HAS_BASE_BSD)
#define BNIO_HAS_BASE 1
#endif

#endif  // BNIO_BASE_CONFIG_H_
