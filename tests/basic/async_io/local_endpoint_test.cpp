#include <bnio/async_io/local/endpoint.h>
#include <bnio/async_io/local/protocol.h>
#include <bnio/async_io/local/socket_view.h>
#include <gtest/gtest.h>

#include <string>

namespace {

using bnio::async_io::local::datagram_protocol;
using bnio::async_io::local::endpoint;
using bnio::async_io::local::endpoint_kind;
using bnio::async_io::local::stream_protocol;

TEST(LocalEndpointTest, default_endpoint_is_unspecified) {
  const endpoint value;
  EXPECT_EQ(value.kind(), endpoint_kind::unspecified);
  EXPECT_TRUE(value.path().empty());
}

TEST(LocalEndpointTest, path_roundtrip) {
  const endpoint value("/tmp/bnio-echo.sock");
  EXPECT_EQ(value.kind(), endpoint_kind::path_name);
  EXPECT_EQ(value.path(), "/tmp/bnio-echo.sock");
}

TEST(LocalEndpointTest, empty_path_is_path_name) {
  const endpoint value("");
  EXPECT_EQ(value.kind(), endpoint_kind::path_name);
  EXPECT_TRUE(value.path().empty());
}

TEST(LocalEndpointTest, max_length_path_is_representable) {
  const std::string path(endpoint::max_path_length, 'a');
  const endpoint value(path);
  EXPECT_EQ(value.kind(), endpoint_kind::path_name);
  EXPECT_EQ(value.path(), path);
}

TEST(LocalEndpointTest, oversize_path_degrades_to_unspecified) {
  const std::string path(endpoint::max_path_length + 1, 'a');
  const endpoint value(path);
  EXPECT_EQ(value.kind(), endpoint_kind::unspecified);
  EXPECT_TRUE(value.path().empty());
}

TEST(LocalEndpointTest, endpoint_is_trivially_copyable) {
  static_assert(std::is_trivially_copyable_v<endpoint>);
  static_assert(std::is_standard_layout_v<endpoint>);

  const endpoint value("/tmp/bnio-echo.sock");
  endpoint copy = value;
  EXPECT_EQ(copy.kind(), endpoint_kind::path_name);
  EXPECT_EQ(copy.path(), "/tmp/bnio-echo.sock");
  copy = endpoint();
  EXPECT_EQ(copy.kind(), endpoint_kind::unspecified);
}

#if defined(BNIO_HAS_ASYNC_IO_LINUX)
TEST(LocalEndpointTest, abstract_roundtrip) {
  const endpoint value = endpoint::abstract("bnio-test-abstract");
  EXPECT_EQ(value.kind(), endpoint_kind::abstract);
  EXPECT_EQ(value.path(), "bnio-test-abstract");
}

TEST(LocalEndpointTest, max_length_abstract_name_is_representable) {
  // The native form spends one sun_path byte on the leading NUL.
  const std::string name(endpoint::max_path_length - 1, 'b');
  const endpoint value = endpoint::abstract(name);
  EXPECT_EQ(value.kind(), endpoint_kind::abstract);
  EXPECT_EQ(value.path(), name);
}

TEST(LocalEndpointTest, oversize_abstract_name_degrades_to_unspecified) {
  const std::string name(endpoint::max_path_length, 'b');
  const endpoint value = endpoint::abstract(name);
  EXPECT_EQ(value.kind(), endpoint_kind::unspecified);
  EXPECT_TRUE(value.path().empty());
}
#endif

TEST(LocalProtocolTest, stream_protocol_triple) {
  const stream_protocol protocol;
  EXPECT_EQ(protocol.type(), SOCK_STREAM);
  EXPECT_EQ(protocol.protocol(), 0);
  EXPECT_EQ(protocol.family(), AF_UNIX);
  static_assert(std::is_same_v<stream_protocol::endpoint, endpoint>);
  static_assert(
      std::is_same_v<stream_protocol::socket, bnio::local::stream_socket>);
  static_assert(
      std::is_same_v<stream_protocol::acceptor, bnio::local::stream_acceptor>);
}

TEST(LocalProtocolTest, datagram_protocol_triple) {
  const datagram_protocol protocol;
  EXPECT_EQ(protocol.type(), SOCK_DGRAM);
  EXPECT_EQ(protocol.protocol(), 0);
  EXPECT_EQ(protocol.family(), AF_UNIX);
  static_assert(std::is_same_v<datagram_protocol::endpoint, endpoint>);
  static_assert(
      std::is_same_v<datagram_protocol::socket, bnio::local::datagram_socket>);
}

TEST(LocalSocketViewTest, view_is_trivial_descriptor_wrapper) {
  static_assert(sizeof(bnio::async_io::local::stream_socket_view) ==
                sizeof(int));
  static_assert(
      std::is_trivially_copyable_v<bnio::async_io::local::stream_socket_view>);
  static_assert(std::is_trivially_copyable_v<
                bnio::async_io::local::datagram_socket_view>);

  const bnio::async_io::local::stream_socket_view invalid;
  EXPECT_FALSE(invalid.valid());
  const bnio::async_io::local::stream_socket_view value(3);
  EXPECT_TRUE(value.valid());
  EXPECT_EQ(value.native_handle(), 3);
}

}  // namespace
