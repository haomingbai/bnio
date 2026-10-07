#include <bnio/async_io/linux/socket_address.h>
#include <bnio/async_io/local/endpoint.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstring>
#include <string>

namespace {

using bnio::async_io::linux_native::make_local_endpoint;
using bnio::async_io::linux_native::socket_address;
using bnio::async_io::local::endpoint;
using bnio::async_io::local::endpoint_kind;

constexpr socklen_t path_offset =
    static_cast<socklen_t>(offsetof(sockaddr_un, sun_path));

TEST(LinuxLocalSocketAddressTest, path_name_encode) {
  const socket_address native(endpoint("/tmp/bnio-echo.sock"));

  EXPECT_TRUE(native.valid());
  EXPECT_EQ(native.family(), AF_UNIX);
  ASSERT_NE(native.data(), nullptr);

  const auto* raw = reinterpret_cast<const sockaddr_un*>(native.data());
  EXPECT_EQ(raw->sun_family, AF_UNIX);
  EXPECT_EQ(std::string_view(raw->sun_path), "/tmp/bnio-echo.sock");
  // Path-name form carries the terminating NUL.
  EXPECT_EQ(native.size(),
            path_offset + std::strlen("/tmp/bnio-echo.sock") + 1);
}

TEST(LinuxLocalSocketAddressTest, abstract_encode) {
  const socket_address native{endpoint::abstract("bnio-test-abstract")};

  EXPECT_TRUE(native.valid());
  EXPECT_EQ(native.family(), AF_UNIX);

  const auto* raw = reinterpret_cast<const sockaddr_un*>(native.data());
  EXPECT_EQ(raw->sun_family, AF_UNIX);
  // Abstract form: leading NUL inside sun_path, name after it, no
  // terminator.
  EXPECT_EQ(raw->sun_path[0], '\0');
  EXPECT_EQ(std::memcmp(raw->sun_path + 1, "bnio-test-abstract",
                        std::strlen("bnio-test-abstract")),
            0);
  EXPECT_EQ(native.size(), path_offset + 1 + std::strlen("bnio-test-abstract"));
}

TEST(LinuxLocalSocketAddressTest, unspecified_endpoint_leaves_storage_empty) {
  const socket_address native{endpoint()};
  EXPECT_FALSE(native.valid());
  EXPECT_EQ(native.family(), AF_UNSPEC);
  EXPECT_EQ(native.data(), nullptr);
  EXPECT_EQ(native.size(), 0);
}

TEST(LinuxLocalSocketAddressTest, full_capacity_path_encodes_without_nul) {
  const std::string path(endpoint::max_path_length, 'a');
  const socket_address native{endpoint(path)};

  EXPECT_TRUE(native.valid());
  EXPECT_EQ(native.size(), path_offset + endpoint::max_path_length);
  const auto* raw = reinterpret_cast<const sockaddr_un*>(native.data());
  EXPECT_EQ(std::memcmp(raw->sun_path, path.data(), path.size()), 0);
}

TEST(LinuxLocalSocketAddressTest, decode_path_name_roundtrip) {
  const socket_address native(endpoint("/tmp/bnio-echo.sock"));
  const auto decoded = make_local_endpoint(native.data(), native.size());

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->kind(), endpoint_kind::path_name);
  EXPECT_EQ(decoded->path(), "/tmp/bnio-echo.sock");
}

TEST(LinuxLocalSocketAddressTest, decode_abstract_roundtrip) {
  const socket_address native{endpoint::abstract("bnio-test-abstract")};
  const auto decoded = make_local_endpoint(native.data(), native.size());

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->kind(), endpoint_kind::abstract);
  EXPECT_EQ(decoded->path(), "bnio-test-abstract");
}

TEST(LinuxLocalSocketAddressTest, decode_unnamed_reports_unspecified) {
  // An unbound socket reports only the family field.
  sockaddr_un raw{};
  raw.sun_family = AF_UNIX;
  const auto decoded =
      make_local_endpoint(reinterpret_cast<const sockaddr*>(&raw),
                          static_cast<socklen_t>(sizeof(sa_family_t)));

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->kind(), endpoint_kind::unspecified);
  EXPECT_TRUE(decoded->path().empty());
}

TEST(LinuxLocalSocketAddressTest, decode_zero_length_reports_unspecified) {
  // recvfrom(2) reports a zero-length source address for a datagram whose
  // sender never bound one.
  sockaddr_un zeroed{};
  const auto decoded =
      make_local_endpoint(reinterpret_cast<const sockaddr*>(&zeroed), 0);

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->kind(), endpoint_kind::unspecified);
  EXPECT_TRUE(decoded->path().empty());
}

TEST(LinuxLocalSocketAddressTest, decode_rejects_wrong_family) {
  sockaddr_in raw{};
  raw.sin_family = AF_INET;
  EXPECT_FALSE(
      make_local_endpoint(reinterpret_cast<const sockaddr*>(&raw), sizeof(raw))
          .has_value());

  EXPECT_FALSE(make_local_endpoint(nullptr, 0).has_value());
}

}  // namespace
