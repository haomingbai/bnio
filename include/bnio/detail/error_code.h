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
 * Reuse a single empty error code on success completion paths instead of
 * default-constructing std::error_code{}, whose default constructor consults
 * system_category() on every call. This value uses the same category as the
 * default constructor, so `ec == std::error_code{}` semantics are preserved.
 */
inline const std::error_code empty_error_code{0, std::system_category()};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_ERROR_CODE_H_
