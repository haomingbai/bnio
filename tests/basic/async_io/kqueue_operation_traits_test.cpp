#include <bupp/async_io/bsd/kqueue_context.h>
#include <gtest/gtest.h>

#include <array>
#include <bexec/operation_state.hpp>

#include "../../support/async_io/kqueue_context_test_support.h"

namespace {

using namespace bupp_async_io_kqueue_test;

struct queue_test_operation : kqueue_operation_base {
  void execute() noexcept override {}
};

struct io_queue_test_operation : kqueue_io_operation_base {
  void prepare(bupp::async_io::bsd_native::kqueue_helper&) noexcept override {}
  void complete_submit_error(int) noexcept override {}
  void execute() noexcept override {}
};

TEST(KqueueOperationTraitsTest, shared_task_queue_separates_cpu_and_io) {
  kqueue_task_queue_state queue;
  queue_test_operation cpu;
  std::array<io_queue_test_operation, 3> io;

  for (io_queue_test_operation& operation : io) {
    queue.push_io(operation);
  }
  queue.push_cpu(cpu);

  EXPECT_EQ(queue.pop_cpu_all(), &cpu);
  EXPECT_EQ(queue.pop_cpu_all(), nullptr);

  std::size_t io_count = 0;
  for (auto* operation = queue.pop_io_all(); operation != nullptr;
       operation = operation->io_next) {
    ++io_count;
  }
  EXPECT_EQ(io_count, io.size());
  EXPECT_EQ(queue.pop_io_all(), nullptr);
  EXPECT_FALSE(queue.closing.load(std::memory_order_acquire));
}

TEST(KqueueOperationTraitsTest, operation_state_concepts) {
  static_assert(bexec::operation_state<kqueue_post_operation<receiver>>);
  static_assert(bexec::operation_state<kqueue_nop_operation<receiver>>);
  static_assert(bexec::operation_state<kqueue_poll_operation<receiver>>);
  static_assert(bexec::operation_state<kqueue_read_operation<receiver>>);
  static_assert(bexec::operation_state<kqueue_write_operation<receiver>>);
}

TEST(KqueueOperationTraitsTest, buffer_operations_own_their_native_io_step) {
  kqueue_context context;
  std::array<char, 16> storage{};
  buffer_view buffer{storage.data(), storage.size()};

  kqueue_read_operation read_operation(context, descriptor_view(1), buffer,
                                       receiver{});
  kqueue_write_operation write_operation(context, descriptor_view(1), buffer,
                                         receiver{});

  EXPECT_TRUE(read_operation.owns_io_step());
  EXPECT_TRUE(write_operation.owns_io_step());

  kqueue_helper read_helper;
  kqueue_helper write_helper;
  read_operation.prepare(read_helper);
  write_operation.prepare(write_helper);
  EXPECT_EQ(read_helper.descriptor(), 1);
  EXPECT_EQ(write_helper.descriptor(), 1);
}

}  // namespace
