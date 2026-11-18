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
 * Public spelling of base::context_view; valid only while the owning
 * ssl::context or base::context_base keeps the SSL_CTX alive. It never owns
 * or frees the handle; installing an owned ALPN closure is ssl::context-only.
 */
using context_view = base::context_view;

}  // namespace bnio::ssl

#endif  // BNIO_SSL_CONTEXT_VIEW_H_
