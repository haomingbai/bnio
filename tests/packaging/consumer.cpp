#include <bnio/async_io/ip/address.h>

int main() {
  const auto address = bnio::async_io::ip::address::loopback_v4();
  return address.is_v4() ? 0 : 1;
}
