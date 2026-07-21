/**
 * @file io_uring_operations.h
 * @brief Aggregate header for all io_uring operation types.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_H_

#include <bnio/async_io/linux/io_uring_operations/core.h>
#include <bnio/async_io/linux/io_uring_operations/file.h>
#include <bnio/async_io/linux/io_uring_operations/helpers.h>
#include <bnio/async_io/linux/io_uring_operations/poll.h>
#include <bnio/async_io/linux/io_uring_operations/resolve.h>
#include <bnio/async_io/linux/io_uring_operations/socket.h>
#include <bnio/async_io/linux/io_uring_operations/views.h>

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_H_
