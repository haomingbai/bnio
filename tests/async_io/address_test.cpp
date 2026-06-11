#include <bupp/async_io/ip/address.h>

#include <cassert>
#include <type_traits>

int main() {
  using bupp::async_io::ip::address;

  constexpr address::v4_bytes k_any_v4{0, 0, 0, 0};
  constexpr address::v4_bytes k_loopback_v4{127, 0, 0, 1};
  constexpr address::v6_bytes k_loopback_v6{0, 0, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 1};

  static_assert(
      std::is_nothrow_move_constructible_v<bupp::async_io::ip::address>);
  static_assert(std::is_nothrow_move_assignable_v<bupp::async_io::ip::address>);

  auto any_v4 = bupp::async_io::ip::address::any_v4();
  assert(any_v4.is_v4());
  assert(!any_v4.is_v6());
  assert(any_v4.to_v4() == 0);
  assert(any_v4.v4() != nullptr);
  assert(*any_v4.v4() == k_any_v4);
  assert(any_v4.v6() == nullptr);

  auto loopback_v4 = bupp::async_io::ip::address::loopback_v4();
  assert(loopback_v4.to_v4() == 0x7f000001U);
  assert(loopback_v4.v4() != nullptr);
  assert(*loopback_v4.v4() == k_loopback_v4);

  auto parsed_v4 = bupp::async_io::ip::make_address("127.0.0.1");
  assert(parsed_v4.has_value());
  assert(parsed_v4->is_v4());
  assert(parsed_v4->to_v4() == 0x7f000001U);

  auto parsed_v4_alias = bupp::async_io::ip::make_addr("0.0.0.0");
  assert(parsed_v4_alias.has_value());
  assert(parsed_v4_alias->to_v4() == 0);

  auto loopback_v6 = bupp::async_io::ip::address::loopback_v6();
  assert(loopback_v6.is_v6());
  assert(loopback_v6.v4() == nullptr);
  assert(loopback_v6.v6() != nullptr);
  assert(*loopback_v6.v6() == k_loopback_v6);

  auto parsed_v6 = bupp::async_io::ip::make_v6_address("::1");
  assert(parsed_v6.has_value());
  assert(parsed_v6->is_v6());
  assert(parsed_v6->v6() != nullptr);
  assert(*parsed_v6->v6() == k_loopback_v6);

  assert(!bupp::async_io::ip::make_address("not an address").has_value());

  bupp::async_io::ip::address reset_address =
      bupp::async_io::ip::address::loopback_v4();
  reset_address.reset();
  assert(reset_address.type() ==
         bupp::async_io::ip::address::version::unspecified);
  assert(reset_address.v4() == nullptr);
  assert(reset_address.v6() == nullptr);

  return 0;
}
