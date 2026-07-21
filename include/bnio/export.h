/**
 * @file export.h
 * @brief Symbol export macros.
 */

#pragma once
#ifndef BNIO_EXPORT_H_
#define BNIO_EXPORT_H_

#if defined(BNIO_SHARED_LIBRARY)
#if defined(_WIN32)
#if defined(BNIO_BUILDING_LIBRARY)
#define BNIO_EXPORT __declspec(dllexport)
#else
#define BNIO_EXPORT __declspec(dllimport)
#endif
#else
#define BNIO_EXPORT __attribute__((visibility("default")))
#endif
#else
#define BNIO_EXPORT
#endif

#endif  // BNIO_EXPORT_H_
