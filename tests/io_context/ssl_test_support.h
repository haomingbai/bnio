#include <bupp/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

[[maybe_unused]] void test_require(bool condition) noexcept {
  assert(condition);
  if (!condition) {
    std::abort();
  }
}

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

struct transfer_state {
  unsigned values = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
  std::size_t bytes = 0;
};

struct transfer_receiver {
  std::shared_ptr<transfer_state> state;
  bupp::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 0;

  void set_value(std::size_t bytes) noexcept {
    ++state->values;
    state->bytes = bytes;
    complete();
  }

  void set_error(std::error_code) noexcept {
    ++state->errors;
    complete();
  }

  void set_stopped() noexcept {
    ++state->stopped;
    complete();
  }

 private:
  void complete() noexcept {
    if (completions != nullptr) {
      ++*completions;
      if (*completions == target && context != nullptr) {
        (void)context->stop();
      }
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

}  // namespace
