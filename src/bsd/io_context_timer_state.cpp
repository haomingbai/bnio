#include <cstddef>
#include <utility>
#include <vector>

#include "bupp/async_io/time.h"
#include "bupp/bsd/detail/io_context_timer_types.h"

namespace bupp {

bool detail::timer_state_data::queue_driver() noexcept {
  if (driver == queued_operation_state::posted) {
    return false;
  }
  driver = queued_operation_state::posted;
  return true;
}

void detail::timer_state_data::complete_driver() noexcept {
  driver = queued_operation_state::idle;
}

void detail::timer_state_data::push_heap(
    detail::timer_heap_item item) noexcept {
  heap.push_back(item);
  sift_heap_up(heap.size() - 1);
}

void detail::timer_state_data::pop_heap() noexcept {
  if (heap.empty()) {
    return;
  }

  const std::size_t last = heap.size() - 1;
  swap_heap_items(0, last);
  heap.pop_back();
  if (!heap.empty()) {
    sift_heap_down(0);
  }
}

void detail::timer_state_data::swap_heap_items(std::size_t first,
                                               std::size_t second) noexcept {
  if (first == second) {
    return;
  }
  std::swap(heap[first], heap[second]);
}

void detail::timer_state_data::sift_heap_up(std::size_t index) noexcept {
  while (index != 0) {
    const std::size_t parent = (index - 1) / 2;
    if (!heap_item_less(index, parent)) {
      break;
    }
    swap_heap_items(index, parent);
    index = parent;
  }
}

void detail::timer_state_data::sift_heap_down(std::size_t index) noexcept {
  for (;;) {
    const std::size_t left = index * 2 + 1;
    const std::size_t right = left + 1;
    std::size_t smallest = index;

    if (left < heap.size() && heap_item_less(left, smallest)) {
      smallest = left;
    }
    if (right < heap.size() && heap_item_less(right, smallest)) {
      smallest = right;
    }
    if (smallest == index) {
      break;
    }
    swap_heap_items(index, smallest);
    index = smallest;
  }
}

bool detail::timer_state_data::heap_item_less(
    std::size_t first, std::size_t second) const noexcept {
  const detail::timer_heap_item& left = heap[first];
  const detail::timer_heap_item& right = heap[second];
  if (left.deadline != right.deadline) {
    return left.deadline < right.deadline;
  }
  return left.timer_id < right.timer_id;
}

}  // namespace bupp
