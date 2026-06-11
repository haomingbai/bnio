#include <bupp/async_io/descriptor_view.h>

#include <cassert>
#include <type_traits>

int main() {
  static_assert(sizeof(bupp::async_io::descriptor_view) == sizeof(int));
  static_assert(std::is_trivially_copyable_v<bupp::async_io::descriptor_view>);
  static_assert(std::is_standard_layout_v<bupp::async_io::descriptor_view>);

  bupp::async_io::descriptor_view invalid;
  assert(!invalid.valid());
  assert(invalid.native_handle() == -1);

  bupp::async_io::descriptor_view descriptor(42);
  assert(descriptor.valid());
  assert(descriptor.native_handle() == 42);

  return 0;
}
