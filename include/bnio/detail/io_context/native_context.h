/**
 * @file native_context.h
 * @brief Platform-native context type alias.
 */

#pragma once
#ifndef BNIO_DETAIL_IO_CONTEXT_NATIVE_CONTEXT_H_
#define BNIO_DETAIL_IO_CONTEXT_NATIVE_CONTEXT_H_

#include <bnio/io_context/config.h>

#if defined(BNIO_HAS_IO_CONTEXT_LINUX)
#include <bnio/async_io/linux/io_uring_context.h>
#elif defined(BNIO_HAS_IO_CONTEXT_BSD)
#include <bnio/async_io/bsd/kqueue_context.h>
#else
#error "bnio::detail::native_context requires a supported native backend."
#endif

namespace bnio::detail {

#if defined(BNIO_HAS_IO_CONTEXT_LINUX)

using native_context = async_io::linux_native::io_uring_context;
using native_context_options = async_io::linux_native::io_uring_context_options;
using native_operation_base = async_io::linux_native::io_uring_operation_base;
using native_io_operation_base =
    async_io::linux_native::io_uring_io_operation_base;
using native_task_queue_state =
    async_io::linux_native::io_uring_task_queue_state;

#elif defined(BNIO_HAS_IO_CONTEXT_BSD)

using native_context = async_io::bsd_native::kqueue_context;
using native_context_options = async_io::bsd_native::kqueue_context_options;
using native_operation_base = async_io::bsd_native::kqueue_operation_base;
using native_io_operation_base = async_io::bsd_native::kqueue_io_operation_base;
using native_task_queue_state = async_io::bsd_native::kqueue_task_queue_state;

#endif

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_IO_CONTEXT_NATIVE_CONTEXT_H_
