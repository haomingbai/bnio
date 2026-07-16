#include <bupp/base/bsd/event.h>
#include <bupp/base/bsd/event_list_view.h>
#include <bupp/base/bsd/kqueue.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <utility>

namespace {

constexpr std::uintptr_t k_user_event_ident = 1;
constexpr std::uint64_t k_user_data = 0x62757070ULL;

TEST(KqueueTest, event_accessors) {
  void* const data = reinterpret_cast<void*>(k_user_data);
  bupp::base::event event(k_user_event_ident, EVFILT_USER, EV_ADD | EV_CLEAR,
                          NOTE_FFNOP, 7, data);

  EXPECT_TRUE(event.ident() == k_user_event_ident);
  EXPECT_TRUE(event.filter() == EVFILT_USER);
  EXPECT_TRUE((event.flags() & EV_ADD) != 0);
  EXPECT_TRUE((event.flags() & EV_CLEAR) != 0);
  EXPECT_TRUE(event.fflags() == NOTE_FFNOP);
  EXPECT_TRUE(event.data() == 7);
  EXPECT_TRUE(event.udata() == data);

  event.set_ident(2);
  event.set_filter(EVFILT_READ);
  event.set_flags(EV_EOF);
  event.set_fflags(0);
  event.set_data(0);
  event.set_udata(nullptr);

  EXPECT_TRUE(event.ident() == 2);
  EXPECT_TRUE(event.filter() == EVFILT_READ);
  EXPECT_TRUE(event.has_eof());
  EXPECT_TRUE(!event.has_error());
  EXPECT_TRUE(event.udata() == nullptr);
}

TEST(KqueueTest, event_list_view) {
  std::array<bupp::base::event, 2> events{};
  bupp::base::event_list_view view(events.data(), events.size());

  EXPECT_TRUE(view.data() == events.data());
  EXPECT_TRUE(view.size() == events.size());
  EXPECT_TRUE(!view.empty());

  view[0].set(k_user_event_ident, EVFILT_USER, EV_ADD, 0, 0, nullptr);
  EXPECT_TRUE(events[0].ident() == k_user_event_ident);
}

TEST(KqueueTest, move_closed_kqueue) {
  bupp::base::kqueue first;
  bupp::base::kqueue second(std::move(first));
  EXPECT_TRUE(!first.is_open());
  EXPECT_TRUE(!second.is_open());

  bupp::base::kqueue third;
  third = std::move(second);
  EXPECT_TRUE(!second.is_open());
  EXPECT_TRUE(!third.is_open());
}

TEST(KqueueTest, open_close_move_kqueue) {
  bupp::base::kqueue queue;
  EXPECT_TRUE(!queue.is_open());
  EXPECT_TRUE(queue.native_fd() < 0);

  const int open_result = queue.open();
  EXPECT_TRUE(open_result == 0);
  EXPECT_TRUE(queue.is_open());
  EXPECT_TRUE(queue.native_fd() >= 0);

  bupp::base::kqueue moved(std::move(queue));
  EXPECT_TRUE(!queue.is_open());
  EXPECT_TRUE(moved.is_open());

  moved.close();
  EXPECT_TRUE(!moved.is_open());
}

TEST(KqueueTest, user_event_wakeup) {
  bupp::base::kqueue queue;
  EXPECT_TRUE(queue.open() == 0);

  void* const data = reinterpret_cast<void*>(k_user_data);
  bupp::base::event add_event(k_user_event_ident, EVFILT_USER,
                              EV_ADD | EV_CLEAR, 0, 0, data);
  EXPECT_TRUE(queue.control(&add_event, 1, nullptr, 0, nullptr) == 0);

  bupp::base::event trigger_event(k_user_event_ident, EVFILT_USER, 0,
                                  NOTE_TRIGGER, 0, data);
  EXPECT_TRUE(queue.control(&trigger_event, 1, nullptr, 0, nullptr) == 0);

  bupp::base::event ready_event;
  const int ready_count = queue.control(nullptr, 0, &ready_event, 1, nullptr);
  EXPECT_TRUE(ready_count == 1);
  EXPECT_TRUE(ready_event.ident() == k_user_event_ident);
  EXPECT_TRUE(ready_event.filter() == EVFILT_USER);
  EXPECT_TRUE(ready_event.udata() == data);
}

TEST(KqueueTest, pipe_readiness) {
  int pipe_fds[2] = {-1, -1};
  EXPECT_TRUE(pipe(pipe_fds) == 0);

  bupp::base::kqueue queue;
  EXPECT_TRUE(queue.open() == 0);

  void* const data = reinterpret_cast<void*>(k_user_data);
  bupp::base::event read_event(static_cast<std::uintptr_t>(pipe_fds[0]),
                               EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, data);
  EXPECT_TRUE(queue.control(&read_event, 1, nullptr, 0, nullptr) == 0);

  constexpr char byte = 'x';
  EXPECT_TRUE(write(pipe_fds[1], &byte, 1) == 1);

  bupp::base::event ready_event;
  const int ready_count = queue.control(nullptr, 0, &ready_event, 1, nullptr);
  EXPECT_TRUE(ready_count == 1);
  EXPECT_TRUE(ready_event.ident() == static_cast<std::uintptr_t>(pipe_fds[0]));
  EXPECT_TRUE(ready_event.filter() == EVFILT_READ);
  EXPECT_TRUE(ready_event.data() >= 1);
  EXPECT_TRUE(ready_event.udata() == data);

  EXPECT_TRUE(close(pipe_fds[0]) == 0);
  EXPECT_TRUE(close(pipe_fds[1]) == 0);
}

TEST(KqueueTest, timeout) {
  bupp::base::kqueue queue;
  EXPECT_TRUE(queue.open() == 0);

  bupp::base::event ready_event;
  timespec timeout{};
  timeout.tv_nsec = 1;

  const int ready_count = queue.control(nullptr, 0, &ready_event, 1, &timeout);
  EXPECT_TRUE(ready_count == 0);
}

}  // namespace
