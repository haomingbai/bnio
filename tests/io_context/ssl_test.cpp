#include <bupp/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {

struct void_receiver {
  void set_value() noexcept {}
  void set_error(std::error_code) noexcept {}
  void set_stopped() noexcept {}
};

struct byte_receiver {
  void set_value(std::size_t) noexcept {}
  void set_error(std::error_code) noexcept {}
  void set_stopped() noexcept {}
};

constexpr std::string_view k_test_certificate = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUDy724cK0qbEW2oFhEuKSzqK96TMwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDYxMTAzMTY0M1oXDTI2MDYx
MjAzMTY0M1owFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA1ApL4VyBg0qkMNExVvws958zfuvXvv9uAEAIwfLai94y
v4nzDq1IdN4LO5RXl2AP6Zgk+lKOV/ifQSWdRgS+WlbS3Sq3WpSjkjHqfaWXTq0Y
DZRhfLKBUV6mX/T0sSoiz80i1XbuanT374/EviaMKLHY5nbGr97/W+E5eGmuHUbM
Qv/bNt9f2yph2I6n2/i+DpB4JdELhhihN7XxRITsjzOG6Uc3a4LRhZZkc3C9dSKO
8XBR/HBr0zrb80iSYkHbwJBY4GUFxy01V+RflcAAo5Vgz2qWXAAoDowPv0/QriPW
b+vF9N+fWgCsB3dRR/61nw9Wa/Om6UVsnyjnSv0OaQIDAQABo1MwUTAdBgNVHQ4E
FgQUrAeAKzS102yRohhWxiN9OkrT7ZkwHwYDVR0jBBgwFoAUrAeAKzS102yRohhW
xiN9OkrT7ZkwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAAF4R
2tuE7rBFmxEdkmJaipTnWSh86I6TyWlE3eSSVjxlf7wLC7iHvWoIfOSoMISSrwUH
FwuppcNONPne31DZ8s9KuVniIw5Jtwqrrsj1M6l54uEAb7KjfWJsvhhJJQMX3N+s
BALJpiHuFOUMP3Qt6hhYTOEkdTkxNJJcnd+Ai/WEI4PwOVYxYhX2vtbnJMCSbn1z
Qmv/ndA6IBdrd6kRMeahjVUd79RipRNTUvoykFo/Pf2qe+VlpvjkW8lfHpIGwpb6
ayXCG6tdCvObruqsFkMfZdr0JqX72vMym1+61jFg7IUZZf2Galtl3nAd6VXS7/b+
EUpa3NSHUio4v40LQg==
-----END CERTIFICATE-----
)";

constexpr std::string_view k_test_private_key = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDUCkvhXIGDSqQw
0TFW/Cz3nzN+69e+/24AQAjB8tqL3jK/ifMOrUh03gs7lFeXYA/pmCT6Uo5X+J9B
JZ1GBL5aVtLdKrdalKOSMep9pZdOrRgNlGF8soFRXqZf9PSxKiLPzSLVdu5qdPfv
j8S+Jowosdjmdsav3v9b4Tl4aa4dRsxC/9s231/bKmHYjqfb+L4OkHgl0QuGGKE3
tfFEhOyPM4bpRzdrgtGFlmRzcL11Io7xcFH8cGvTOtvzSJJiQdvAkFjgZQXHLTVX
5F+VwACjlWDPapZcACgOjA+/T9CuI9Zv68X0359aAKwHd1FH/rWfD1Zr86bpRWyf
KOdK/Q5pAgMBAAECggEAIoFJb1MvJTcaiHImWg4f4CzZQ6xr16I325UQB8W2EEA4
mGhBtA/5REFU6R1e8px4gm4WiGCyVrj363FMSlZfxpIt7r0yiKw7AQGb89XkTTKI
QT917MWcmynwn5lcT084yoGKi1u2+P5vUV3fKYVa1g146yoFc52xhtlcEY76/TrZ
8cjrcdHkvryskjFePRBTPTqr5evDyN9Wkog9+M5PfLQfVZToeKmEaw3CP22iKQnT
XYOejmsNcj+HqPCZsyP6JG/x+JL3OuXXq0wWnYr2tkXhfEigxiZoAj9RubIbTo06
bUZhdGlyPMUfQbbIDY3/E5HaJvBFIW5IhsegXJt8rQKBgQD3B4EjeaMQze8/UWS2
h1MRAXdHlGHcXi+1cLkA+MsH/5s5HMVXGZz5b1LdIuEzAJ0xs9LVTodmPU+7EusP
EcAQVYo6zK/k53TxEB++9LCtOwyyheIc0cfJE908sPg5YnbmS3JS8w7fMrzhj/Md
GIsDGPJY5F6A/UPLDHcd6B7WLQKBgQDbvYSCXzJSSgSSWWm/cHd13SuAgBAzQFfh
3FIA+9CkUaxVp8ZgldSPHj85fPvJQiFXLvX+yCTzq9KupxKfRIlfiGEKwlEBT78F
tztfddULEM7TxBo3L3GNLYc2Q/c/53NgUIbl7T138v+wpGQ94AIr++8jBHLsHVx+
l1qImVTarQKBgFICRsf9MLp6c4vEvLewE06Y+v1jcF2VUydcJb8B2X1tSR3bxFPX
J/rTD2Jkmviwon8GoN65tE+n2RlU/X5COU3y5/H/VAGdKYCCBtgBKcpIyT1XHyrM
JhRGKPNmGPIME0b/ExQgpvZIRNZpUJ9/L1823/XM0ublraTyHXVrQxl9AoGAC8MV
OLVHyEfV/s9ybaDjhBeWoIY6V8P18E0Oxqa0AFeu1dbpM3pRqmeAEt+xypAToMsO
t9iWwcRMvrSKtqPAhrCSITVNiLhwDSpFr1JrWPBJYeR5UsLjXR82wZzZuz30Ww90
aRJN3AHR1e62vukitKADqOgwDptzvAL2AaHTfPECgYEAh9L1950MRzS4ATqlkI2g
gX5iMKBXwxZSOKzdXwtOUoX3g5kA13Grqtd6p3u4J08m6yj5HE6A8WFPL5poH5dd
1hhnq2ljRPkv7jgynx1aHYRelYScFEfrrgmY5o3Q9DaWkffJ1NKAechorSLHcNSK
0iRf69BHQ0Idy7XKp2tP11k=
-----END PRIVATE KEY-----
)";

struct handshake_state {
  unsigned values = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
};

struct handshake_receiver {
  std::shared_ptr<handshake_state> state;
  bupp::io_context* context = nullptr;

  void set_value() noexcept {
    ++state->values;
    if (state->values == 2 && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code) noexcept {
    ++state->errors;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    ++state->stopped;
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct test_certificate_files {
  std::filesystem::path directory;
  std::filesystem::path certificate;
  std::filesystem::path private_key;

  test_certificate_files() {
    directory = std::filesystem::temp_directory_path() /
                ("bupp_ssl_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    certificate = directory / "cert.pem";
    private_key = directory / "key.pem";
    std::ofstream(certificate) << k_test_certificate;
    std::ofstream(private_key) << k_test_private_key;
  }

  ~test_certificate_files() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

void test_ssl_sender_concepts() {
  using stream_type = bupp::ssl_stream<bupp::tcp_socket>;
  using scheduler_type = bupp::io_context::post_scheduler;
  using handshake_sender =
      decltype(std::declval<stream_type&>().async_handshake(
          std::declval<scheduler_type>(), bupp::ssl_handshake_type::client));
  using handshake_direct_sender =
      decltype(std::declval<stream_type&>().async_handshake_direct(
          std::declval<scheduler_type>(), bupp::ssl_handshake_type::client));
  using shutdown_sender = decltype(std::declval<stream_type&>().async_shutdown(
      std::declval<scheduler_type>()));
  using shutdown_direct_sender =
      decltype(std::declval<stream_type&>().async_shutdown_direct(
          std::declval<scheduler_type>()));
  using read_sender = decltype(std::declval<stream_type&>().async_read(
      std::declval<scheduler_type>(), std::declval<bupp::mutable_buffer>()));
  using read_direct_sender =
      decltype(std::declval<stream_type&>().async_read_direct(
          std::declval<scheduler_type>(),
          std::declval<bupp::mutable_buffer>()));
  using write_sender = decltype(std::declval<stream_type&>().async_write(
      std::declval<scheduler_type>(), std::declval<bupp::const_buffer>()));
  using write_direct_sender =
      decltype(std::declval<stream_type&>().async_write_direct(
          std::declval<scheduler_type>(), std::declval<bupp::const_buffer>()));

  static_assert(bexec::sender<handshake_sender>);
  static_assert(bexec::sender<handshake_direct_sender>);
  static_assert(bexec::sender<shutdown_sender>);
  static_assert(bexec::sender<shutdown_direct_sender>);
  static_assert(bexec::sender<read_sender>);
  static_assert(bexec::sender<read_direct_sender>);
  static_assert(bexec::sender<write_sender>);
  static_assert(bexec::sender<write_direct_sender>);

  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<handshake_sender>(), void_receiver{}))>);
  static_assert(
      bexec::operation_state<decltype(bexec::connect(
          std::declval<handshake_direct_sender>(), void_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<shutdown_sender>(), void_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<shutdown_direct_sender>(), void_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<read_sender>(), byte_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<read_direct_sender>(), byte_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<write_sender>(), byte_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<write_direct_sender>(), byte_receiver{}))>);
}

void test_ssl_raii_objects_construct() {
  bupp::ssl_context context;
  assert(context.valid());
  context.set_verify_mode(SSL_VERIFY_NONE);

  bupp::tcp_socket socket(-1);
  bupp::ssl_stream stream(std::move(socket), context);
  assert(stream.valid());
  assert(stream.native_handle() == stream.get_native_handle());
  assert(stream.lowest_layer().get_native_handle() == -1);
}

template <bool DirectSubmit>
void test_socketpair_handshake_is_io_context_driven() {
  bupp::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  test_certificate_files files;

  bupp::ssl_context server_context(bupp::ssl_context_method::tls_server);
  assert(server_context.valid());
  assert(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  assert(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  assert(!server_context.check_private_key());

  bupp::ssl_context client_context(bupp::ssl_context_method::tls_client);
  assert(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

  bupp::ssl_stream client{bupp::tcp_socket(sockets[0]), client_context};
  bupp::ssl_stream server{bupp::tcp_socket(sockets[1]), server_context};

  auto state = std::make_shared<handshake_state>();
  handshake_receiver client_receiver{state, &context};
  handshake_receiver server_receiver{state, &context};

  auto client_sender = [&] {
    if constexpr (DirectSubmit) {
      return client.async_handshake_direct(scheduler,
                                           bupp::ssl_handshake_type::client);
    } else {
      return client.async_handshake(scheduler,
                                    bupp::ssl_handshake_type::client);
    }
  }();
  auto server_sender = [&] {
    if constexpr (DirectSubmit) {
      return server.async_handshake_direct(scheduler,
                                           bupp::ssl_handshake_type::server);
    } else {
      return server.async_handshake(scheduler,
                                    bupp::ssl_handshake_type::server);
    }
  }();

  auto client_operation =
      bexec::connect(std::move(client_sender), std::move(client_receiver));
  auto server_operation =
      bexec::connect(std::move(server_sender), std::move(server_receiver));

  bexec::start(client_operation);
  bexec::start(server_operation);
  if constexpr (DirectSubmit) {
    assert(scheduler.queued_io_size() == 0);
  } else {
    assert(scheduler.queued_io_size() != 0);
  }
  context.run();

  assert(state->values == 2);
  assert(state->errors == 0);
  assert(state->stopped == 0);
}

}  // namespace

int main() {
  test_ssl_sender_concepts();
  test_ssl_raii_objects_construct();
  test_socketpair_handshake_is_io_context_driven<false>();
  test_socketpair_handshake_is_io_context_driven<true>();
  return 0;
}
