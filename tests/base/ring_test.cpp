#include <bupp/base/completion_queue_entry.h>
#include <bupp/base/params.h>
#include <bupp/base/ring.h>
#include <bupp/base/submission_queue_entry.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <utility>

namespace {

constexpr std::uint64_t k_user_data = 0x62757070ULL;

bool is_unsupported_ring_error(int result) {
  return result == -ENOSYS || result == -EPERM || result == -EACCES;
}

void test_move_closed_ring() {
  bupp::base::ring first;
  bupp::base::ring second(std::move(first));
  assert(!first.is_open());
  assert(!second.is_open());

  bupp::base::ring third;
  third = std::move(second);
  assert(!second.is_open());
  assert(!third.is_open());
}

void test_nop_round_trip() {
  bupp::base::ring ring;
  bupp::base::params params;

  const int init_result = ring.queue_init_params(8, params);
  if (init_result < 0) {
    assert(is_unsupported_ring_error(init_result));
    assert(!ring.is_open());
    return;
  }

  assert(ring.is_open());
  assert(ring.raw() != nullptr);

  bupp::base::submission_queue_entry sqe = ring.get_sqe();
  assert(sqe.raw() != nullptr);

  sqe.prep_nop();
  sqe.set_data64(k_user_data);

  const int submit_result = ring.submit();
  assert(submit_result >= 0);

  bupp::base::completion_queue_entry cqe;
  const int wait_result = ring.wait_cqe(cqe);
  assert(wait_result == 0);
  assert(cqe.raw() != nullptr);
  assert(cqe.res() == 0);
  assert(cqe.get_data64() == k_user_data);
  assert(!cqe.has_more());

  ring.cqe_seen(cqe);

  bupp::base::ring moved(std::move(ring));
  assert(!ring.is_open());
  assert(moved.is_open());
}

void test_consume_ready_cqes_respects_window() {
  bupp::base::ring ring;

  const int init_result = ring.queue_init(8);
  if (init_result < 0) {
    assert(is_unsupported_ring_error(init_result));
    assert(!ring.is_open());
    return;
  }

  constexpr unsigned k_count = 3;
  for (unsigned index = 0; index < k_count; ++index) {
    bupp::base::submission_queue_entry sqe = ring.get_sqe();
    assert(sqe.raw() != nullptr);
    sqe.prep_nop();
    sqe.set_data64(k_user_data + index);
  }

  const int submit_result = ring.submit_and_wait(k_count);
  assert(submit_result >= 0);

  unsigned seen_count = 0;
  std::uint64_t seen_sum = 0;
  const auto record_cqe = [&seen_count, &seen_sum](
                              bupp::base::completion_queue_entry cqe) noexcept {
    assert(cqe.raw() != nullptr);
    assert(cqe.res() == 0);
    seen_sum += cqe.get_data64();
    ++seen_count;
  };

  const unsigned first_batch = ring.consume_ready_cqes(2, record_cqe);
  assert(first_batch == 2);
  assert(seen_count == 2);

  const unsigned second_batch = ring.consume_ready_cqes(2, record_cqe);
  assert(second_batch == 1);
  assert(seen_count == k_count);
  assert(seen_sum == (k_user_data * k_count) + 0 + 1 + 2);
}

}  // namespace

int main() {
  test_move_closed_ring();
  test_nop_round_trip();
  test_consume_ready_cqes_respects_window();
  return 0;
}
