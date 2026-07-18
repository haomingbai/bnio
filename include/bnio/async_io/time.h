#pragma once
#ifndef BNIO_ASYNC_IO_TIME_H_
#define BNIO_ASYNC_IO_TIME_H_

#include <chrono>

namespace bnio::async_io {

/**
 * Monotonic clock used by async I/O scheduling.
 */
using steady_clock = std::chrono::steady_clock;

/**
 * Alias for the default async I/O scheduling clock.
 */
using clock = steady_clock;

/**
 * Wall-clock type for APIs that explicitly need system time.
 */
using system_clock = std::chrono::system_clock;

/**
 * Duration type used by the default async I/O scheduling clock.
 */
using duration = clock::duration;

/**
 * Time point used by the default async I/O scheduling clock.
 */
using time_point = clock::time_point;

/**
 * Wall-clock time point for APIs that explicitly need system time.
 */
using system_time_point = system_clock::time_point;

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_TIME_H_
