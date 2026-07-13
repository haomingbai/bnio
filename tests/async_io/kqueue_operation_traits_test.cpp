#include <bupp/async_io/bsd/kqueue_context.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <cassert>

#include "kqueue_context_test_support.h"

namespace {

using namespace bupp_async_io_kqueue_test;

void test_operation_state_concepts() {
  static_assert(bexec::operation_state<kqueue_post_operation<receiver>>);
  static_assert(bexec::operation_state<kqueue_nop_operation<receiver>>);
  static_assert(bexec::operation_state<kqueue_poll_operation<receiver>>);
  static_assert(bexec::operation_state<kqueue_read_operation<receiver>>);
  static_assert(bexec::operation_state<kqueue_write_operation<receiver>>);
}

void test_buffer_operations_expose_buffer_view_by_value() {
  kqueue_context context;
  std::array<char, 16> storage{};
  buffer_view buffer{storage.data(), storage.size()};

  kqueue_read_operation read_operation(context, descriptor_view(1), buffer,
                                       receiver{});
  kqueue_write_operation write_operation(context, descriptor_view(1), buffer,
                                         receiver{});

  const buffer_view read_data = read_operation.get_data();
  const buffer_view write_data = write_operation.get_data();
  assert(read_data.data == storage.data());
  assert(read_data.size == storage.size());
  assert(write_data.data == storage.data());
  assert(write_data.size == storage.size());
}

}  // namespace

int main() {
  test_operation_state_concepts();
  test_buffer_operations_expose_buffer_view_by_value();
  return 0;
}
