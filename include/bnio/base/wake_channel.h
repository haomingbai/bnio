/**
 * @file wake_channel.h
 * @brief Platform dispatcher for base::wake_channel.
 */

#pragma once
#ifndef BNIO_BASE_WAKE_CHANNEL_H_
#define BNIO_BASE_WAKE_CHANNEL_H_

#include <bnio/base/config.h>

#if defined(BNIO_HAS_BASE_LINUX)
#include <bnio/base/linux/wake_channel.h>
#elif defined(BNIO_HAS_BASE_BSD)
#include <bnio/base/bsd/wake_channel.h>
#endif

#endif  // BNIO_BASE_WAKE_CHANNEL_H_
