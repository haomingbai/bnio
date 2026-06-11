#include <bupp/async_io/buffer_view.h>

#include <cassert>
#include <cstddef>
#include <type_traits>

int main() {
  static_assert(std::is_trivially_copyable_v<bupp::async_io::buffer_view>);

  char data[8]{};
  bupp::async_io::buffer_view buffer{data, sizeof(data)};

  assert(buffer.data == data);
  assert(buffer.size == sizeof(data));

  return 0;
}
