#include <gtest/gtest.h>

#include "../../support/io_context/io_context_runtime_test_support.h"

// Interface tests for bnio::async_io::random_access_file. The positioned
// overloads used here (async_read_some/async_write_some/async_read/
// async_write with an explicit offset) are provided by a follow-up change;
// this file is not wired into any CMakeLists target yet.

namespace {

constexpr std::string_view kContent = "0123456789abcdef";

// Creates an unlinked temporary file pre-filled with kContent.
[[nodiscard]] int make_content_file() {
  std::string path = "/tmp/bnio-io-context-raf-XXXXXX";
  const int fd = ::mkstemp(path.data());
  EXPECT_TRUE(fd >= 0);
  EXPECT_EQ(::unlink(path.c_str()), 0);
  EXPECT_EQ(::pwrite(fd, kContent.data(), kContent.size(), 0),
            static_cast<ssize_t>(kContent.size()));
  return fd;
}

TEST(IoContextRandomAccessFileTest, view_wraps_descriptor_without_ownership) {
  bnio::async_io::random_access_file empty;
  EXPECT_FALSE(empty.valid());
  EXPECT_EQ(empty.native_handle(), -1);

  bnio::async_io::descriptor_view descriptor(42);
  bnio::async_io::random_access_file file(descriptor);
  EXPECT_TRUE(file.valid());
  EXPECT_EQ(file.native_handle(), 42);
}

// Detection must live in a templated entity: at block scope a
// requires-expression over an invalid call is ill-formed, not false
// ([expr.prim.req] — the expression only soft-fails where template
// substitution into the requirement can occur).
template <class Scheduler, class File, class Buffer>
concept offsetless_read_some_invocable =
    requires(Scheduler& scheduler, File& file, Buffer&& buffer) {
      bnio::async_read_some(scheduler, file, std::forward<Buffer>(buffer));
    };

template <class Scheduler, class File, class Buffer>
concept offsetless_write_some_invocable =
    requires(Scheduler& scheduler, File& file, Buffer&& buffer) {
      bnio::async_write_some(scheduler, file, std::forward<Buffer>(buffer));
    };

template <class Scheduler, class File, class Buffer>
concept offsetless_read_invocable =
    requires(Scheduler& scheduler, File& file, Buffer&& buffer) {
      bnio::async_read(scheduler, file, std::forward<Buffer>(buffer));
    };

template <class Scheduler, class File, class Buffer>
concept offsetless_write_invocable =
    requires(Scheduler& scheduler, File& file, Buffer&& buffer) {
      bnio::async_write(scheduler, file, std::forward<Buffer>(buffer));
    };

// Contract: the offset parameter is mandatory on the random access
// overloads — there is no offset-less fallback. If an overload without an
// offset for random_access_file ever appears, these assertions fail to
// compile.
TEST(IoContextRandomAccessFileTest, offset_parameter_is_mandatory) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  std::array<char, 8> bytes{};
  bnio::async_io::random_access_file file;

  using scheduler_type = decltype(scheduler);
  using buffer_type = decltype(bnio::buffer(bytes));

  static_assert(
      !offsetless_read_some_invocable<scheduler_type, decltype(file),
                                      buffer_type>);
  static_assert(
      !offsetless_write_some_invocable<scheduler_type, decltype(file),
                                       buffer_type>);
  static_assert(
      !offsetless_read_invocable<scheduler_type, decltype(file), buffer_type>);
  static_assert(
      !offsetless_write_invocable<scheduler_type, decltype(file), buffer_type>);
  static_assert(requires {
    bnio::async_read_some(scheduler, file, bnio::buffer(bytes),
                          std::uint64_t{0});
  });
}

TEST(IoContextRandomAccessFileTest,
     positioned_read_is_independent_of_file_position) {
  const int fd = make_content_file();

  // Move the kernel file position away from the read offsets; positioned
  // reads must observe file content, not the kernel position.
  EXPECT_EQ(::lseek(fd, 4, SEEK_SET), static_cast<off_t>(4));

  std::array<char, 4> first{};
  std::array<char, 4> second{};
  for (auto* bytes : {&first, &second}) {
    bnio::io_context context;
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = bnio::async_read_some(
        scheduler, bnio::async_io::random_access_file(fd),
        bnio::buffer(*bytes), std::uint64_t{0});
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    EXPECT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, bytes->size());
  }

  // Two reads at the same offset are deterministic and return the file
  // content at that offset regardless of the kernel file position.
  EXPECT_TRUE(std::memcmp(first.data(), second.data(), first.size()) == 0);
  EXPECT_TRUE(std::memcmp(first.data(), kContent.data(), first.size()) == 0);

  EXPECT_EQ(::close(fd), 0);
}

TEST(IoContextRandomAccessFileTest, positioned_write_overwrites_at_offset) {
  const int fd = make_content_file();

  constexpr std::string_view patch = "WXYZ";
  {
    bnio::io_context context;
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = bnio::async_write_some(
        scheduler, bnio::async_io::random_access_file(fd),
        bnio::buffer(patch), std::uint64_t{4});
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    EXPECT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, patch.size());
  }

  std::array<char, 16> verified{};
  EXPECT_EQ(::pread(fd, verified.data(), verified.size(), 0),
            static_cast<ssize_t>(verified.size()));
  EXPECT_TRUE(std::memcmp(verified.data() + 4, patch.data(), patch.size()) ==
              0);
  // Surrounding bytes are untouched.
  EXPECT_TRUE(std::memcmp(verified.data(), kContent.data(), 4) == 0);
  EXPECT_TRUE(std::memcmp(verified.data() + 8, kContent.data() + 8, 8) == 0);

  EXPECT_EQ(::close(fd), 0);
}

TEST(IoContextRandomAccessFileTest,
     positioned_io_does_not_advance_kernel_file_position) {
  const int fd = make_content_file();

  constexpr off_t parked = 3;
  EXPECT_EQ(::lseek(fd, parked, SEEK_SET), parked);

  {
    bnio::io_context context;
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    std::array<char, 4> bytes{};
    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = bnio::async_read_some(
        scheduler, bnio::async_io::random_access_file(fd), bnio::buffer(bytes),
        std::uint64_t{8});
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    EXPECT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, bytes.size());
    EXPECT_TRUE(std::memcmp(bytes.data(), kContent.data() + 8, bytes.size()) ==
                0);
  }
  EXPECT_EQ(::lseek(fd, 0, SEEK_CUR), parked);

  {
    bnio::io_context context;
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    constexpr std::string_view patch = "xy";
    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = bnio::async_write_some(
        scheduler, bnio::async_io::random_access_file(fd),
        bnio::buffer(patch), std::uint64_t{0});
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    EXPECT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, patch.size());
  }
  EXPECT_EQ(::lseek(fd, 0, SEEK_CUR), parked);

  EXPECT_EQ(::close(fd), 0);
}

TEST(IoContextRandomAccessFileTest, read_all_and_write_all_fill_exact_region) {
  const int fd = make_content_file();

  {
    bnio::io_context context;
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    // Read-all at offset: the full buffer must be filled from offset 8.
    std::array<char, 8> bytes{};
    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = bnio::async_read(scheduler,
                                   bnio::async_io::random_access_file(fd),
                                   bnio::buffer(bytes), std::uint64_t{8});
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    EXPECT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, bytes.size());
    EXPECT_TRUE(std::memcmp(bytes.data(), kContent.data() + 8, bytes.size()) ==
                0);
  }

  {
    bnio::io_context context;
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    // Write-all at offset: the full payload lands in the region starting at
    // offset 8, looping on partial writes if necessary.
    constexpr std::string_view payload = "ABCDEFGH";
    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = bnio::async_write(scheduler,
                                    bnio::async_io::random_access_file(fd),
                                    bnio::buffer(payload), std::uint64_t{8});
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    EXPECT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, payload.size());
  }

  std::array<char, 16> verified{};
  EXPECT_EQ(::pread(fd, verified.data(), verified.size(), 0),
            static_cast<ssize_t>(verified.size()));
  constexpr std::string_view payload = "ABCDEFGH";
  EXPECT_TRUE(std::memcmp(verified.data() + 8, payload.data(), payload.size()) ==
              0);
  EXPECT_TRUE(std::memcmp(verified.data(), kContent.data(), 8) == 0);

  EXPECT_EQ(::close(fd), 0);
}

}  // namespace
