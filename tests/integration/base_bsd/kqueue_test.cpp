#include <bnio/base/bsd/event.h>
#include <bnio/base/bsd/event_list_view.h>
#include <bnio/base/bsd/kqueue.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <type_traits>
#include <utility>

namespace {

constexpr std::uintptr_t k_user_event_ident = 1;
constexpr std::uint64_t k_user_data = 0x62757070ULL;

TEST(KqueueTest, event_accessors) {
  void* const data = reinterpret_cast<void*>(k_user_data);
  bnio::base::event event(k_user_event_ident, EVFILT_USER, EV_ADD | EV_CLEAR,
                          NOTE_FFNOP, 7, data);

  EXPECT_EQ(event.ident(), k_user_event_ident);
  EXPECT_EQ(event.filter(), EVFILT_USER);
  EXPECT_TRUE((event.flags() & EV_ADD) != 0);
  EXPECT_TRUE((event.flags() & EV_CLEAR) != 0);
  EXPECT_EQ(event.fflags(), NOTE_FFNOP);
  EXPECT_EQ(event.data(), 7);
  EXPECT_EQ(event.udata(), data);

  event.set_ident(2);
  event.set_filter(EVFILT_READ);
  event.set_flags(EV_EOF);
  event.set_fflags(0);
  event.set_data(0);
  event.set_udata(nullptr);

  EXPECT_EQ(event.ident(), 2);
  EXPECT_EQ(event.filter(), EVFILT_READ);
  EXPECT_TRUE(event.has_eof());
  EXPECT_FALSE(event.has_error());
  EXPECT_EQ(event.udata(), nullptr);
}

TEST(KqueueTest, event_list_view) {
  std::array<bnio::base::event, 2> events{};
  bnio::base::event_list_view view(events.data(), events.size());

  EXPECT_EQ(view.data(), events.data());
  EXPECT_EQ(view.size(), events.size());
  EXPECT_FALSE(view.empty());

  view[0].set(k_user_event_ident, EVFILT_USER, EV_ADD, 0, 0, nullptr);
  EXPECT_EQ(events[0].ident(), k_user_event_ident);

  // Iterator support: begin()/end() alias the viewed array.
  EXPECT_EQ(view.begin(), events.data());
  EXPECT_EQ(view.end(), events.data() + events.size());

  std::size_t counted = 0;
  for (auto& ev : view) {
    ev.set(k_user_event_ident, EVFILT_USER, EV_ADD, 0, 0, nullptr);
    ++counted;
  }
  EXPECT_EQ(counted, events.size());
  EXPECT_EQ(events[1].ident(), k_user_event_ident);

  const bnio::base::event_list_view const_view = view;
  static_assert(std::is_same_v<bnio::base::event_list_view::const_iterator,
                               const bnio::base::event*>);
  EXPECT_EQ(const_view.begin(), events.data());
  EXPECT_EQ(const_view.end(), events.data() + events.size());
}

TEST(KqueueTest, move_closed_kqueue) {
  bnio::base::kqueue first;
  bnio::base::kqueue second(std::move(first));
  EXPECT_FALSE(first.is_open());
  EXPECT_FALSE(second.is_open());

  bnio::base::kqueue third;
  third = std::move(second);
  EXPECT_FALSE(second.is_open());
  EXPECT_FALSE(third.is_open());
}

TEST(KqueueTest, open_close_move_kqueue) {
  bnio::base::kqueue queue;
  EXPECT_FALSE(queue.is_open());
  EXPECT_LT(queue.native_fd(), 0);

  const int open_result = queue.open();
  EXPECT_EQ(open_result, 0);
  EXPECT_TRUE(queue.is_open());
  EXPECT_TRUE(queue.native_fd() >= 0);

  bnio::base::kqueue moved(std::move(queue));
  EXPECT_FALSE(queue.is_open());
  EXPECT_TRUE(moved.is_open());

  moved.close();
  EXPECT_FALSE(moved.is_open());
}

TEST(KqueueTest, user_event_wakeup) {
  bnio::base::kqueue queue;
  EXPECT_EQ(queue.open(), 0);

  void* const data = reinterpret_cast<void*>(k_user_data);
  bnio::base::event add_event(k_user_event_ident, EVFILT_USER,
                              EV_ADD | EV_CLEAR, 0, 0, data);
  EXPECT_EQ(queue.control(&add_event, 1, nullptr, 0, nullptr), 0);

  bnio::base::event trigger_event(k_user_event_ident, EVFILT_USER, 0,
                                  NOTE_TRIGGER, 0, data);
  EXPECT_EQ(queue.control(&trigger_event, 1, nullptr, 0, nullptr), 0);

  bnio::base::event ready_event;
  const int ready_count = queue.control(nullptr, 0, &ready_event, 1, nullptr);
  EXPECT_EQ(ready_count, 1);
  EXPECT_EQ(ready_event.ident(), k_user_event_ident);
  EXPECT_EQ(ready_event.filter(), EVFILT_USER);
  EXPECT_EQ(ready_event.udata(), data);
}

TEST(KqueueTest, pipe_readiness) {
  int pipe_fds[2] = {-1, -1};
  EXPECT_EQ(pipe(pipe_fds), 0);

  bnio::base::kqueue queue;
  EXPECT_EQ(queue.open(), 0);

  void* const data = reinterpret_cast<void*>(k_user_data);
  bnio::base::event read_event(static_cast<std::uintptr_t>(pipe_fds[0]),
                               EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, data);
  EXPECT_EQ(queue.control(&read_event, 1, nullptr, 0, nullptr), 0);

  constexpr char byte = 'x';
  EXPECT_EQ(write(pipe_fds[1], &byte, 1), 1);

  bnio::base::event ready_event;
  const int ready_count = queue.control(nullptr, 0, &ready_event, 1, nullptr);
  EXPECT_EQ(ready_count, 1);
  EXPECT_EQ(ready_event.ident(), static_cast<std::uintptr_t>(pipe_fds[0]));
  EXPECT_EQ(ready_event.filter(), EVFILT_READ);
  EXPECT_TRUE(ready_event.data() >= 1);
  EXPECT_EQ(ready_event.udata(), data);

  EXPECT_EQ(close(pipe_fds[0]), 0);
  EXPECT_EQ(close(pipe_fds[1]), 0);
}

TEST(KqueueTest, timeout) {
  bnio::base::kqueue queue;
  EXPECT_EQ(queue.open(), 0);

  bnio::base::event ready_event;
  timespec timeout{};
  timeout.tv_nsec = 1;

  const int ready_count = queue.control(nullptr, 0, &ready_event, 1, &timeout);
  EXPECT_EQ(ready_count, 0);
}

}  // namespace
