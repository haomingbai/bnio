#include "io_context_runtime_test_support.h"

namespace {

void test_poll_observes_pipe_readiness() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int descriptors[2] = {-1, -1};
  assert(::pipe2(descriptors, O_CLOEXEC) == 0);

  poll_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = bupp::async_poll(
      scheduler, bupp::async_io::descriptor_view(descriptors[0]),
      static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  constexpr char byte = 'q';
  assert(::write(descriptors[1], &byte, sizeof(byte)) ==
         static_cast<ssize_t>(sizeof(byte)));
  context.run();

  assert(state->signal == signal_kind::value);
  assert((static_cast<unsigned>(state->size) & static_cast<unsigned>(POLLIN)) !=
         0);

  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

}  // namespace

int main() {
  test_poll_observes_pipe_readiness();
  return 0;
}
