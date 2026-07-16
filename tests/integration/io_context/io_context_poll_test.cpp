#include <gtest/gtest.h>

#include "../../support/io_context/io_context_runtime_test_support.h"

namespace {

TEST(IoContextPollTest, poll_observes_pipe_readiness) {
  bupp::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int descriptors[2] = {-1, -1};
#if defined(BUPP_SYSTEM_LINUX)
  EXPECT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);
#else
  EXPECT_EQ(::pipe(descriptors), 0);
  EXPECT_EQ(::fcntl(descriptors[0], F_SETFD, FD_CLOEXEC), 0);
  EXPECT_EQ(::fcntl(descriptors[1], F_SETFD, FD_CLOEXEC), 0);
#endif

  poll_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = bupp::async_poll(
      scheduler, bupp::async_io::descriptor_view(descriptors[0]),
      static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  constexpr char byte = 'q';
  EXPECT_EQ(::write(descriptors[1], &byte, sizeof(byte)),
            static_cast<ssize_t>(sizeof(byte)));
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_TRUE((static_cast<unsigned>(state->size) &
               static_cast<unsigned>(POLLIN)) != 0);

  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

}  // namespace
