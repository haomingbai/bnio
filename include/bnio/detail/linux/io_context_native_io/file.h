/**
 * @file file.h
 * @brief Linux native streaming and positioned file I/O models.
 */

#ifndef BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FILE_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FILE_H_

#include <bnio/detail/linux/io_context_native_io/descriptor_file.h>
#include <bnio/detail/linux/io_context_native_io/random_access_file.h>

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FILE_H_
