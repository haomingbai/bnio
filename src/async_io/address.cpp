#include <bupp/async_io/ip/address.h>

namespace bupp::async_io::ip {
namespace {

constexpr address::v4_bytes k_any_v4{0, 0, 0, 0};
constexpr address::v4_bytes k_loopback_v4{127, 0, 0, 1};
constexpr address::v6_bytes k_any_v6{};
constexpr address::v6_bytes k_loopback_v6{0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 1};

address::v4_bytes host_order_to_v4_bytes(std::uint32_t value) noexcept {
  return address::v4_bytes{
      static_cast<std::uint8_t>((value >> 24U) & 0xffU),
      static_cast<std::uint8_t>((value >> 16U) & 0xffU),
      static_cast<std::uint8_t>((value >> 8U) & 0xffU),
      static_cast<std::uint8_t>(value & 0xffU),
  };
}

std::uint32_t v4_bytes_to_host_order(const address::v4_bytes& value) noexcept {
  return (static_cast<std::uint32_t>(value[0]) << 24U) |
         (static_cast<std::uint32_t>(value[1]) << 16U) |
         (static_cast<std::uint32_t>(value[2]) << 8U) |
         static_cast<std::uint32_t>(value[3]);
}

}  // namespace

address::address() noexcept = default;

address address::any_v4() noexcept { return v4(k_any_v4); }

address address::loopback_v4() noexcept { return v4(k_loopback_v4); }

address address::v4(std::uint32_t value) noexcept {
  address result;
  result.set_v4(value);
  return result;
}

address address::v4(v4_bytes value) noexcept {
  address result;
  result.set_v4(value);
  return result;
}

address address::any_v6() noexcept { return v6(k_any_v6); }

address address::loopback_v6() noexcept { return v6(k_loopback_v6); }

address address::v6(v6_bytes value) noexcept {
  address result;
  result.set_v6(value);
  return result;
}

void address::reset() noexcept {
  type_ = version::unspecified;
  v4_ = {};
  v6_ = {};
}

void address::set_v4(std::uint32_t value) noexcept {
  set_v4(host_order_to_v4_bytes(value));
}

void address::set_v4(v4_bytes value) noexcept {
  type_ = version::v4;
  v4_ = value;
  v6_ = {};
}

void address::set_v6(v6_bytes value) noexcept {
  type_ = version::v6;
  v4_ = {};
  v6_ = value;
}

address::version address::type() const noexcept { return type_; }

bool address::is_v4() const noexcept { return type_ == version::v4; }

bool address::is_v6() const noexcept { return type_ == version::v6; }

const address::v4_bytes* address::v4() const noexcept {
  return is_v4() ? &v4_ : nullptr;
}

const address::v6_bytes* address::v6() const noexcept {
  return is_v6() ? &v6_ : nullptr;
}

std::uint32_t address::to_v4() const noexcept {
  return is_v4() ? v4_bytes_to_host_order(v4_) : 0;
}

}  // namespace bupp::async_io::ip
