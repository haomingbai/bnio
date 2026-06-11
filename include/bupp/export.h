#pragma once
#ifndef BUPP_EXPORT_H_
#define BUPP_EXPORT_H_

#if defined(BUPP_SHARED_LIBRARY)
#if defined(_WIN32)
#if defined(BUPP_BUILDING_LIBRARY)
#define BUPP_EXPORT __declspec(dllexport)
#else
#define BUPP_EXPORT __declspec(dllimport)
#endif
#else
#define BUPP_EXPORT __attribute__((visibility("default")))
#endif
#else
#define BUPP_EXPORT
#endif

#endif  // BUPP_EXPORT_H_
