/**
 * @file error_code.h
 * @brief Shared empty error code for async-I/O completion hot paths.
 */

#pragma once
#ifndef BNIO_DETAIL_ERROR_CODE_H_
#define BNIO_DETAIL_ERROR_CODE_H_

#include <system_error>

namespace bnio::detail {

/**
 * Shared empty error code for async-I/O completion hot paths.
 *
 * Avoids default-constructing std::error_code{}, which consults
 * system_category() on every call. Same category as the default
 * constructor, so `ec == std::error_code{}` still holds.
 */
inline const std::error_code empty_error_code{0, std::system_category()};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_ERROR_CODE_H_
