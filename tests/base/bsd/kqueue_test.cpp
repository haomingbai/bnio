#include <bupp/base/bsd/event.h>
#include <bupp/base/bsd/event_list_view.h>
#include <bupp/base/bsd/kqueue.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <utility>

namespace {

constexpr std::uintptr_t k_user_event_ident = 1;
constexpr std::uint64_t k_user_data = 0x62757070ULL;

void test_event_accessors() {
  void* const data = reinterpret_cast<void*>(k_user_data);
  bupp::base::event event(k_user_event_ident, EVFILT_USER, EV_ADD | EV_CLEAR,
                          NOTE_FFNOP, 7, data);

  assert(event.ident() == k_user_event_ident);
  assert(event.filter() == EVFILT_USER);
  assert((event.flags() & EV_ADD) != 0);
  assert((event.flags() & EV_CLEAR) != 0);
  assert(event.fflags() == NOTE_FFNOP);
  assert(event.data() == 7);
  assert(event.udata() == data);

  event.set_ident(2);
  event.set_filter(EVFILT_READ);
  event.set_flags(EV_EOF);
  event.set_fflags(0);
  event.set_data(0);
  event.set_udata(nullptr);

  assert(event.ident() == 2);
  assert(event.filter() == EVFILT_READ);
  assert(event.has_eof());
  assert(!event.has_error());
  assert(event.udata() == nullptr);
}

void test_event_list_view() {
  std::array<bupp::base::event, 2> events{};
  bupp::base::event_list_view view(events.data(), events.size());

  assert(view.data() == events.data());
  assert(view.size() == events.size());
  assert(!view.empty());

  view[0].set(k_user_event_ident, EVFILT_USER, EV_ADD, 0, 0, nullptr);
  assert(events[0].ident() == k_user_event_ident);
}

void test_move_closed_kqueue() {
  bupp::base::kqueue first;
  bupp::base::kqueue second(std::move(first));
  assert(!first.is_open());
  assert(!second.is_open());

  bupp::base::kqueue third;
  third = std::move(second);
  assert(!second.is_open());
  assert(!third.is_open());
}

void test_open_close_move_kqueue() {
  bupp::base::kqueue queue;
  assert(!queue.is_open());
  assert(queue.native_fd() < 0);

  const int open_result = queue.open();
  assert(open_result == 0);
  assert(queue.is_open());
  assert(queue.native_fd() >= 0);

  bupp::base::kqueue moved(std::move(queue));
  assert(!queue.is_open());
  assert(moved.is_open());

  moved.close();
  assert(!moved.is_open());
}

void test_user_event_wakeup() {
  bupp::base::kqueue queue;
  assert(queue.open() == 0);

  void* const data = reinterpret_cast<void*>(k_user_data);
  bupp::base::event add_event(k_user_event_ident, EVFILT_USER,
                              EV_ADD | EV_CLEAR, 0, 0, data);
  assert(queue.control(&add_event, 1, nullptr, 0, nullptr) == 0);

  bupp::base::event trigger_event(k_user_event_ident, EVFILT_USER, 0,
                                  NOTE_TRIGGER, 0, data);
  assert(queue.control(&trigger_event, 1, nullptr, 0, nullptr) == 0);

  bupp::base::event ready_event;
  const int ready_count = queue.control(nullptr, 0, &ready_event, 1, nullptr);
  assert(ready_count == 1);
  assert(ready_event.ident() == k_user_event_ident);
  assert(ready_event.filter() == EVFILT_USER);
  assert(ready_event.udata() == data);
}

void test_pipe_readiness() {
  int pipe_fds[2] = {-1, -1};
  assert(pipe(pipe_fds) == 0);

  bupp::base::kqueue queue;
  assert(queue.open() == 0);

  void* const data = reinterpret_cast<void*>(k_user_data);
  bupp::base::event read_event(static_cast<std::uintptr_t>(pipe_fds[0]),
                               EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, data);
  assert(queue.control(&read_event, 1, nullptr, 0, nullptr) == 0);

  constexpr char byte = 'x';
  assert(write(pipe_fds[1], &byte, 1) == 1);

  bupp::base::event ready_event;
  const int ready_count = queue.control(nullptr, 0, &ready_event, 1, nullptr);
  assert(ready_count == 1);
  assert(ready_event.ident() == static_cast<std::uintptr_t>(pipe_fds[0]));
  assert(ready_event.filter() == EVFILT_READ);
  assert(ready_event.data() >= 1);
  assert(ready_event.udata() == data);

  assert(close(pipe_fds[0]) == 0);
  assert(close(pipe_fds[1]) == 0);
}

void test_timeout() {
  bupp::base::kqueue queue;
  assert(queue.open() == 0);

  bupp::base::event ready_event;
  timespec timeout{};
  timeout.tv_nsec = 1;

  const int ready_count = queue.control(nullptr, 0, &ready_event, 1, &timeout);
  assert(ready_count == 0);
}

}  // namespace

int main() {
  test_event_accessors();
  test_event_list_view();
  test_move_closed_kqueue();
  test_open_close_move_kqueue();
  test_user_event_wakeup();
  test_pipe_readiness();
  test_timeout();
  return 0;
}
