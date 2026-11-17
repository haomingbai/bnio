/**
 * @file context_view.h
 * @brief Public non-owning view over a native OpenSSL SSL_CTX.
 */

#pragma once
#ifndef BNIO_SSL_CONTEXT_VIEW_H_
#define BNIO_SSL_CONTEXT_VIEW_H_

#include <bnio/ssl/base/context_view.h>

namespace bnio::ssl {

/**
 * Non-owning view over a native OpenSSL SSL_CTX.
 *
 * This is the public spelling of base::context_view. It is valid only while
 * the owning ssl::context (or ssl::base::context_base) keeps the native
 * SSL_CTX alive. It never owns or frees the handle, and installing an owned
 * ALPN closure remains an ssl::context operation.
 */
using context_view = base::context_view;

}  // namespace bnio::ssl

#endif  // BNIO_SSL_CONTEXT_VIEW_H_
