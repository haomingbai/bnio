#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cstdint>
#include <span>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../../support/io_context/ssl_test_support.h"

namespace {

template <class Owner>
void self_move_assign(Owner& owner) {
  owner = std::move(owner);
}

// ALPN callback whose first parameter takes the context view with the given
// value category. The framework invokes user callbacks with an lvalue context
// view (see context::alpn_closure::make_bound), so the concept must accept
// by-value, const lvalue reference, and non-const lvalue reference parameters
// and reject an rvalue reference parameter.
template <class ContextArg>
struct context_arg_alpn_callback {
  int operator()(ContextArg, bnio::ssl::stream_view,
                 bnio::ssl::context::alpn_out&,
                 std::span<const unsigned char>) const;
};

template <class ContextArg>
concept alpn_concept_accepts_context_arg =
    bnio::ssl::detail::alpn_callback_invocable<
        context_arg_alpn_callback<ContextArg>, bnio::ssl::stream_view,
        bnio::ssl::context::alpn_out>;

// The public overload applies the same constraint: a callable that cannot be
// invoked with an lvalue context view is rejected at overload resolution.
template <class ContextArg>
concept set_alpn_callback_accepts_context_arg = requires(
    bnio::ssl::context& ctx, context_arg_alpn_callback<ContextArg> callback) {
  ctx.set_alpn_callback(callback);
};

static_assert(alpn_concept_accepts_context_arg<bnio::ssl::context_view>);
static_assert(alpn_concept_accepts_context_arg<const bnio::ssl::context_view&>);
static_assert(alpn_concept_accepts_context_arg<bnio::ssl::context_view&>);
static_assert(!alpn_concept_accepts_context_arg<bnio::ssl::context_view&&>);

static_assert(set_alpn_callback_accepts_context_arg<bnio::ssl::context_view>);
static_assert(
    set_alpn_callback_accepts_context_arg<const bnio::ssl::context_view&>);
static_assert(set_alpn_callback_accepts_context_arg<bnio::ssl::context_view&>);
static_assert(
    !set_alpn_callback_accepts_context_arg<bnio::ssl::context_view&&>);

// Normalizes a member function pointer into result type, parameter types, and
// noexcept-ness so context_base forwarders and their context_view targets can
// be compared without depending on the owning class type.
template <class MemPtr>
struct member_signature;

template <class C, class R, class... Args>
struct member_signature<R (C::*)(Args...)> {
  using result = R;
  using arguments = std::tuple<Args...>;
  static constexpr bool is_noexcept = false;
};

template <class C, class R, class... Args>
struct member_signature<R (C::*)(Args...) noexcept> {
  using result = R;
  using arguments = std::tuple<Args...>;
  static constexpr bool is_noexcept = true;
};

template <class C, class R, class... Args>
struct member_signature<R (C::*)(Args...) const> {
  using result = R;
  using arguments = std::tuple<Args...>;
  static constexpr bool is_noexcept = false;
};

template <class C, class R, class... Args>
struct member_signature<R (C::*)(Args...) const noexcept> {
  using result = R;
  using arguments = std::tuple<Args...>;
  static constexpr bool is_noexcept = true;
};

template <class OwnerPtr, class ViewPtr>
constexpr bool same_member_signature =
    std::is_same_v<typename member_signature<OwnerPtr>::result,
                   typename member_signature<ViewPtr>::result> &&
    std::is_same_v<typename member_signature<OwnerPtr>::arguments,
                   typename member_signature<ViewPtr>::arguments> &&
    member_signature<OwnerPtr>::is_noexcept ==
        member_signature<ViewPtr>::is_noexcept;

// context_base keeps the full non-owning configuration surface and forwards
// each method to context_view with the exact same signature, return type, and
// noexcept specification.
#define BNIO_ASSERT_CONTEXT_BASE_FORWARDS(name)                              \
  static_assert(                                                             \
      same_member_signature<decltype(&bnio::ssl::base::context_base::name),  \
                            decltype(&bnio::ssl::base::context_view::name)>, \
      "context_base::" #name " must match context_view::" #name)

BNIO_ASSERT_CONTEXT_BASE_FORWARDS(use_certificate_chain_file);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(use_private_key_file);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(check_private_key);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_verify_mode);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_alpn_select_cb);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(use_certificate_file);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_verify_depth);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_options);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(get_options);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_cipher_list);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_ciphersuites);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(load_verify_locations);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_default_verify_paths);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_min_proto_version);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_max_proto_version);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_session_cache_mode);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_session_id_context);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_keylog_callback);
BNIO_ASSERT_CONTEXT_BASE_FORWARDS(set_alpn_protos);

#undef BNIO_ASSERT_CONTEXT_BASE_FORWARDS

static_assert(std::is_same_v<bnio::ssl::base::context_base::alpn_select_cb,
                             bnio::ssl::base::context_view::alpn_select_cb>);

TEST(SslConceptTest, ssl_sender_concepts) {
  using stream_type = bnio::ssl::tcp::stream<bnio::tcp_socket>;
  using scheduler_type = bnio::io_context::post_scheduler;
  using handshake_sender =
      decltype(std::declval<stream_type&>().async_handshake(
          std::declval<scheduler_type>(), bnio::ssl::handshake_type::client));
  using shutdown_sender = decltype(std::declval<stream_type&>().async_shutdown(
      std::declval<scheduler_type>()));
  using read_sender = decltype(std::declval<stream_type&>().async_read(
      std::declval<scheduler_type>(), std::declval<bnio::mutable_buffer>()));
  using read_some_sender =
      decltype(std::declval<stream_type&>().async_read_some(
          std::declval<scheduler_type>(),
          std::declval<bnio::mutable_buffer>()));
  using write_sender = decltype(std::declval<stream_type&>().async_write(
      std::declval<scheduler_type>(), std::declval<bnio::const_buffer>()));
  using write_some_sender =
      decltype(std::declval<stream_type&>().async_write_some(
          std::declval<scheduler_type>(), std::declval<bnio::const_buffer>()));

  static_assert(bexec::sender<handshake_sender>);
  static_assert(bexec::sender<shutdown_sender>);
  static_assert(bexec::sender<read_sender>);
  static_assert(bexec::sender<read_some_sender>);
  static_assert(bexec::sender<write_sender>);
  static_assert(bexec::sender<write_some_sender>);

  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<handshake_sender>(), void_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<shutdown_sender>(), void_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<read_sender>(), byte_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<read_some_sender>(), byte_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<write_sender>(), byte_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<write_some_sender>(), byte_receiver{}))>);
}

TEST(SslConceptTest, ssl_raii_objects_construct) {
  bnio::ssl::context context;
  EXPECT_TRUE(context.valid());
  context.set_verify_mode(SSL_VERIFY_NONE);

  bnio::tcp_socket socket(-1);
  bnio::ssl::tcp::stream stream(std::move(socket), context);
  EXPECT_TRUE(stream.valid());
  EXPECT_EQ(stream.native_handle(), stream.get_native_handle());
  EXPECT_EQ(stream.lowest_layer().get_native_handle(), -1);
}

// context_view is a trivially copyable, non-owning handle. It default
// constructs invalid, echoes an explicitly supplied native handle, compares by
// handle, and forwards the same configuration surface as ssl::context.
TEST(SslConceptTest, context_view_is_non_owning_and_forwards_configuration) {
  static_assert(std::is_trivially_copyable_v<bnio::ssl::context_view>);
  static_assert(std::is_nothrow_copy_constructible_v<bnio::ssl::context_view>);
  static_assert(std::is_default_constructible_v<bnio::ssl::context_view>);

  bnio::ssl::context_view invalid;
  EXPECT_FALSE(invalid.valid());
  EXPECT_EQ(invalid.native_handle(), nullptr);

  bnio::ssl::context context(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(context.valid());

  const bnio::ssl::context_view view(context.native_handle());
  EXPECT_TRUE(view.valid());
  EXPECT_EQ(view.native_handle(), context.native_handle());
  EXPECT_TRUE(view == context.view());

  // The owning context is the primary stream entry point. The explicit
  // context_view overload remains available for callers that only hold a
  // non-owning handle.
  static_assert(
      std::is_constructible_v<bnio::ssl::tcp::stream<bnio::tcp_socket>,
                              bnio::tcp_socket, bnio::ssl::context&>);
  static_assert(
      std::is_constructible_v<bnio::ssl::tcp::stream<bnio::tcp_socket>,
                              bnio::tcp_socket,
                              bnio::ssl::base::context_base&>);
  static_assert(
      std::is_constructible_v<bnio::ssl::tcp::stream<bnio::tcp_socket>,
                              bnio::tcp_socket, bnio::ssl::context_view>);
  bnio::ssl::tcp::stream view_stream(bnio::tcp_socket(-1), view);
  EXPECT_TRUE(view_stream.valid());

  const bnio::ssl::context_view copied = view;
  EXPECT_TRUE(copied == view);

  // Owner forwarding and explicit view calls both reach the native context.
  context.set_verify_mode(SSL_VERIFY_NONE);
  context.set_verify_depth(5);
  context.set_options(SSL_OP_NO_TICKET);
  EXPECT_NE((context.get_options() & SSL_OP_NO_TICKET), std::uint64_t{0});
  context.view().set_verify_mode(SSL_VERIFY_PEER);
  EXPECT_EQ(context.view().native_handle(), view.native_handle());
  EXPECT_TRUE(context.valid());

  ERR_clear_error();
  const std::error_code chain_error =
      context.use_certificate_chain_file("/bnio-test/missing-chain.pem");
  EXPECT_TRUE(chain_error);
  EXPECT_EQ(chain_error.category(), bnio::ssl::openssl_error_category());

  ERR_clear_error();
  const std::error_code view_key_error =
      context.view().use_private_key_file("/bnio-test/missing-key.pem");
  EXPECT_TRUE(view_key_error);
  EXPECT_EQ(view_key_error.category(), bnio::ssl::openssl_error_category());
}

TEST(SslConceptTest, context_base_keeps_configuration_surface_routed_to_view) {
  bnio::ssl::base::context_base context(TLS_server_method());
  ASSERT_TRUE(context.valid());
  EXPECT_EQ(context.native_handle(), context.view().native_handle());

  // Owner forwarding reaches the same native SSL_CTX state a direct view call
  // observes: context_view is the sole implementation.
  context.set_verify_mode(SSL_VERIFY_PEER);
  EXPECT_EQ(SSL_CTX_get_verify_mode(context.native_handle()), SSL_VERIFY_PEER);
  context.set_verify_depth(7);
  EXPECT_EQ(SSL_CTX_get_verify_depth(context.native_handle()), 7);

  context.set_options(SSL_OP_NO_TICKET);
  EXPECT_NE((context.get_options() & SSL_OP_NO_TICKET), std::uint64_t{0});
  EXPECT_EQ(context.get_options(), context.view().get_options());

  const unsigned char protocols[] = {2, 'h', '2'};
  EXPECT_FALSE(context.set_alpn_protos(protocols, sizeof(protocols)));
  EXPECT_FALSE(context.set_cipher_list("DEFAULT"));
  EXPECT_FALSE(context.set_ciphersuites("TLS_AES_128_GCM_SHA256"));
  EXPECT_FALSE(context.set_min_proto_version(TLS1_2_VERSION));
  EXPECT_FALSE(context.set_max_proto_version(TLS1_3_VERSION));

  context.set_session_cache_mode(SSL_SESS_CACHE_SERVER);
  EXPECT_EQ(SSL_CTX_get_session_cache_mode(context.native_handle()),
            SSL_SESS_CACHE_SERVER);

  const unsigned char sid[] = {1, 2, 3, 4};
  EXPECT_FALSE(context.set_session_id_context(sid, sizeof(sid)));

  // The typedef re-exported from context_view is usable for a native
  // registration through the owner.
  bnio::ssl::base::context_base::alpn_select_cb const alpn_cb = nullptr;
  context.set_alpn_select_cb(alpn_cb, nullptr);
  context.set_keylog_callback(nullptr);

  ERR_clear_error();
  const std::error_code chain_error =
      context.use_certificate_chain_file("/bnio-test/missing-chain.pem");
  EXPECT_TRUE(chain_error);
  EXPECT_EQ(chain_error.category(), bnio::ssl::openssl_error_category());

  ERR_clear_error();
  const std::error_code certificate_error = context.use_certificate_file(
      "/bnio-test/missing-cert.pem", SSL_FILETYPE_PEM);
  EXPECT_TRUE(certificate_error);
  EXPECT_EQ(certificate_error.category(), bnio::ssl::openssl_error_category());

  ERR_clear_error();
  const std::error_code key_error =
      context.use_private_key_file("/bnio-test/missing-key.pem");
  EXPECT_TRUE(key_error);
  EXPECT_EQ(key_error.category(), bnio::ssl::openssl_error_category());

  ERR_clear_error();
  const std::error_code locations_error =
      context.load_verify_locations("/bnio-test/missing-ca.pem", nullptr);
  EXPECT_TRUE(locations_error);
  EXPECT_EQ(locations_error.category(), bnio::ssl::openssl_error_category());
}

TEST(SslConceptTest, ssl_context_errors_and_ownership) {
  const std::error_code synthetic_error = bnio::ssl::make_openssl_error(1);
  EXPECT_EQ(synthetic_error.value(), 1);
  EXPECT_EQ(synthetic_error.category(), bnio::ssl::openssl_error_category());
  EXPECT_EQ(std::string_view(synthetic_error.category().name()), "openssl");
  EXPECT_FALSE(synthetic_error.message().empty());

  bnio::ssl::context generic_context(bnio::ssl::context_method::tls);
  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  EXPECT_TRUE(generic_context.valid());
  EXPECT_TRUE(client_context.valid());
  EXPECT_TRUE(server_context.valid());

  SSL_CTX* const client_handle = client_context.native_handle();
  bnio::ssl::context moved_context(std::move(client_context));
  EXPECT_FALSE(client_context.valid());
  EXPECT_EQ(moved_context.native_handle(), client_handle);

  generic_context = std::move(moved_context);
  EXPECT_FALSE(moved_context.valid());
  EXPECT_EQ(generic_context.native_handle(), client_handle);
  self_move_assign(generic_context);
  EXPECT_EQ(generic_context.native_handle(), client_handle);

  ERR_clear_error();
  const std::error_code certificate_error =
      server_context.use_certificate_chain_file(
          "/bnio-test/path-that-does-not-exist/cert.pem");
  EXPECT_TRUE(certificate_error);
  EXPECT_EQ(certificate_error.category(), bnio::ssl::openssl_error_category());

  ERR_clear_error();
  const std::error_code key_error = server_context.use_private_key_file(
      "/bnio-test/path-that-does-not-exist/key.pem");
  EXPECT_TRUE(key_error);
  EXPECT_EQ(key_error.category(), bnio::ssl::openssl_error_category());

  ERR_clear_error();
  const std::error_code mismatch_error = server_context.check_private_key();
  EXPECT_TRUE(mismatch_error);
  EXPECT_EQ(mismatch_error.category(), bnio::ssl::openssl_error_category());
}

}  // namespace
