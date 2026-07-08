#pragma once
#ifndef BUPP_CONFIG_SYSTEM_H_
#define BUPP_CONFIG_SYSTEM_H_

#if defined(__linux__)
#define BUPP_SYSTEM_LINUX 1
#define BUPP_SYSTEM_POSIX 1
#elif defined(__APPLE__) && defined(__MACH__)
#define BUPP_SYSTEM_DARWIN 1
#define BUPP_SYSTEM_POSIX 1
#elif defined(_WIN32)
#define BUPP_SYSTEM_WINDOWS 1
#else
#define BUPP_SYSTEM_UNKNOWN 1
#endif

#endif  // BUPP_CONFIG_SYSTEM_H_
