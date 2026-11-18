// file_cat: asynchronous `cat` as a repeat_until pipeline over streaming
// descriptor I/O; an EOF/error round zeroes the chunk and ends the loop.

#include <bnio/bnio.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <bexec/bexec.hpp>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <system_error>

namespace {

constexpr std::size_t k_chunk = 64 * 1024;

// Shared state for the pipeline. All completion handlers run on the
// io_context thread, so plain members suffice.
struct cat_state {
  int in_fd = -1;
  std::size_t chunk = k_chunk;  // bytes read in the current round (0 = done)
  std::size_t total = 0;        // bytes handed to stdout
  std::error_code error;        // first error observed, if any
  std::array<char, k_chunk> buf{};
};

// Terminal receiver: the loop finished (or was stopped); tell the context.
struct cat_receiver {
  bnio::io_context* ctx;

  void set_value(std::size_t) noexcept { ctx->stop(); }

  void set_error(std::exception_ptr e) noexcept {
    // Only reachable if the framework fails to store/connect a child; bnio
    // I/O itself reports errors through error_code values.
    try {
      if (e) std::rethrow_exception(e);
    } catch (const std::exception& ex) {
      std::cerr << "unexpected failure: " << ex.what() << '\n';
    }
    ctx->stop();
  }

  void set_stopped() noexcept { ctx->stop(); }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << (argc > 0 ? argv[0] : "file_cat") << " <file>\n";
    return 2;
  }

  // A closed stdout reader (e.g. `file_cat big | head`) must surface EPIPE
  // through error_code instead of killing the process.
  std::signal(SIGPIPE, SIG_IGN);

  const int in_fd = ::open(argv[1], O_RDONLY);
  if (in_fd < 0) {
    std::cerr << "cannot open '" << argv[1]
              << "': " << std::generic_category().message(errno) << '\n';
    return 1;
  }

  bnio::io_context ctx;
  if (!ctx.is_open()) {
    std::cerr << "io_context unavailable\n";
    (void)::close(in_fd);
    return 1;
  }

  cat_state state;
  state.in_fd = in_fd;

  const auto io = ctx.get_post_scheduler();
  const auto file = bnio::async_io::descriptor_view(state.in_fd);
  const auto out = bnio::async_io::descriptor_view(STDOUT_FILENO);

  // Read a chunk, then stream it. let_value needs one return-sender type, so
  // EOF/error rounds use a zero-byte write, and chunk == 0 ends the loop.
  auto read_round = [&]() {
    return io.async_read_some(file, bnio::buffer(state.buf)) |
           bexec::let_value([&](std::error_code ec, std::size_t n) {
             if (ec) {
               if (!state.error) state.error = ec;
               n = 0;
             }
             state.chunk = n;
             // Streaming writes work for every stdout kind: a regular file
             // advances its own kernel position, pipes/tty take bytes in order.
             return io.async_write(
                 out, bnio::const_buffer(state.buf.data(), state.chunk));
           }) |
           bexec::then(
               [&](std::error_code ec, std::size_t) noexcept -> std::size_t {
                 if (ec) {
                   if (!state.error) state.error = ec;
                   state.chunk = 0;
                 } else {
                   state.total += state.chunk;
                 }
                 return state.chunk;
               });
  };

  auto operation = bexec::connect(
      bexec::repeat_until(read_round, [&] { return state.chunk == 0; }),
      cat_receiver{&ctx});
  bexec::start(operation);

  ctx.run();

  (void)::close(in_fd);
  if (state.error) {
    std::cerr << "file_cat: " << state.error.message() << " (after "
              << state.total << " bytes)\n";
    return 1;
  }
  return 0;
}
