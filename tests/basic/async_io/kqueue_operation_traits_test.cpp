#include <bnio/async_io/bsd/kqueue_context.h>
#include <gtest/gtest.h>

#include <array>
#include <bexec/completion_signatures.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "../../support/async_io/kqueue_context_test_support.h"

namespace {

using namespace bnio_async_io_kqueue_test;

struct queue_test_operation : kqueue_operation_base {
  void execute() noexcept override {}
};

struct io_queue_test_operation : kqueue_io_operation_base {
  void prepare(bnio::async_io::bsd_native::kqueue_helper&) noexcept override {}
  void complete_submit_error(int) noexcept override {}
  void complete_submit_stopped() noexcept override {}
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
  EXPECT_FALSE(queue.life_state.load(std::memory_order_acquire));
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

TEST(KqueueOperationTraitsTest, file_io_splits_streaming_and_positioned) {
  using bnio::async_io::random_access_file;

  // Streaming: descriptor_view without offset advances the kernel file
  // position; positioned: random_access_file with an explicit offset.
  using stream_read_sender =
      decltype(std::declval<kqueue_context&>().async_read(descriptor_view{},
                                                          buffer_view{}));
  using stream_write_sender =
      decltype(std::declval<kqueue_context&>().async_write(
          descriptor_view{}, static_cast<const void*>(nullptr),
          std::size_t{0}));
  using positioned_read_sender =
      decltype(std::declval<kqueue_context&>().async_read(
          random_access_file{}, buffer_view{}, std::uint64_t{0}));
  using positioned_write_sender =
      decltype(std::declval<kqueue_context&>().async_write(
          random_access_file{}, static_cast<const void*>(nullptr),
          std::size_t{0}, std::uint64_t{0}));

  static_assert(bexec::sender<stream_read_sender>);
  static_assert(bexec::sender<stream_write_sender>);
  static_assert(bexec::sender<positioned_read_sender>);
  static_assert(bexec::sender<positioned_write_sender>);

  using expected_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;
  static_assert(
      std::is_same_v<typename stream_read_sender::completion_signatures,
                     expected_signatures>);
  static_assert(
      std::is_same_v<typename stream_write_sender::completion_signatures,
                     expected_signatures>);
  static_assert(
      std::is_same_v<typename positioned_read_sender::completion_signatures,
                     expected_signatures>);
  static_assert(
      std::is_same_v<typename positioned_write_sender::completion_signatures,
                     expected_signatures>);
}

}  // namespace
