/**
 * @file local_endpoint.cpp
 * @brief Local (AF_UNIX) endpoint implementation.
 */

#include <bnio/async_io/local/endpoint.h>

#include <cstring>

namespace bnio::async_io::local {

endpoint::endpoint() noexcept = default;

endpoint::endpoint(std::string_view path) noexcept {
  if (path.size() > max_path_length) {
    return;
  }
  kind_ = endpoint_kind::path_name;
  length_ = path.size();
  if (!path.empty()) {
    std::memcpy(path_, path.data(), path.size());
  }
}

#if defined(BNIO_HAS_ASYNC_IO_LINUX)
endpoint endpoint::abstract(std::string_view name) noexcept {
  // The native form spends sun_path[0] on the leading NUL, so only
  // max_path_length - 1 name bytes fit.
  if (name.size() + 1 > max_path_length) {
    return endpoint();
  }
  endpoint value;
  value.kind_ = endpoint_kind::abstract;
  value.length_ = name.size();
  if (!name.empty()) {
    std::memcpy(value.path_, name.data(), name.size());
  }
  return value;
}
#endif

endpoint_kind endpoint::kind() const noexcept { return kind_; }

std::string_view endpoint::path() const noexcept {
  return std::string_view(path_, length_);
}

}  // namespace bnio::async_io::local
