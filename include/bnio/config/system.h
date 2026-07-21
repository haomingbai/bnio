/**
 * @file system.h
 * @brief Platform detection and feature macros.
 */

#pragma once
#ifndef BNIO_CONFIG_SYSTEM_H_
#define BNIO_CONFIG_SYSTEM_H_

#if defined(__linux__)
#define BNIO_SYSTEM_LINUX 1
#define BNIO_SYSTEM_POSIX 1
#elif defined(__APPLE__) && defined(__MACH__)
#define BNIO_SYSTEM_DARWIN 1
#define BNIO_SYSTEM_BSD 1
#define BNIO_SYSTEM_POSIX 1
#elif defined(__FreeBSD__)
#define BNIO_SYSTEM_FREEBSD 1
#define BNIO_SYSTEM_BSD 1
#define BNIO_SYSTEM_POSIX 1
#elif defined(_WIN32)
#define BNIO_SYSTEM_WINDOWS 1
#else
#define BNIO_SYSTEM_UNKNOWN 1
#endif

#endif  // BNIO_CONFIG_SYSTEM_H_
