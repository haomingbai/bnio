/**
 * @file io_context_timer_state.cpp
 * @brief Pairing-heap timer state management for the internal timer queue.
 */

#include <utility>

#include "bnio/async_io/time.h"
#include "bnio/detail/io_context/timer_types.h"

namespace bnio {

void detail::timer_state_data::push_heap(detail::timer_slot& timer) noexcept {
  if (timer.active) {
    return;
  }

  timer.previous = nullptr;
  timer.child = nullptr;
  timer.next = nullptr;
  timer.active = true;
  // Pairing-heap insert: meld the new single-node heap into the root.
  heap = meld(heap, &timer);
}

detail::timer_slot* detail::timer_state_data::pop_heap() noexcept {
  timer_slot* const timer = heap;
  if (timer != nullptr) {
    erase_heap(*timer);
  }
  return timer;
}

void detail::timer_state_data::erase_heap(detail::timer_slot& timer) noexcept {
  if (!timer.active) {
    return;
  }

  // Cut the node out of the sibling list. If it has a parent, fix the
  // parent's child pointer. Otherwise it is the root and we replace it
  // with its next sibling.
  if (timer.previous != nullptr) {
    if (timer.previous->child == &timer) {
      timer.previous->child = timer.next;
    } else {
      timer.previous->next = timer.next;
    }
    if (timer.next != nullptr) {
      timer.next->previous = timer.previous;
    }
  } else {
    if (heap != &timer) {
      return;
    }
    heap = timer.next;
    if (heap != nullptr) {
      heap->previous = nullptr;
    }
  }

  // Replace the removed node with the merged result of its children (the
  // standard pairing-heap delete-min). Meld the replacement back into the
  // root.
  timer_slot* const replacement = merge_pairs(timer.child);
  timer.previous = nullptr;
  timer.child = nullptr;
  timer.next = nullptr;
  timer.active = false;
  heap = meld(heap, replacement);
}

void detail::timer_state_data::push_inactive(
    detail::timer_slot& timer) noexcept {
  if (timer.active || &timer == inactive || timer.previous != nullptr ||
      timer.next != nullptr) {
    return;
  }

  // Insert at the head of the inactive (already-expired) doubly-linked list.
  timer.child = nullptr;
  timer.previous = nullptr;
  timer.next = inactive;
  if (inactive != nullptr) {
    inactive->previous = &timer;
  }
  inactive = &timer;
}

void detail::timer_state_data::erase_inactive(
    detail::timer_slot& timer) noexcept {
  if (timer.active) {
    return;
  }

  // Unlink from the inactive doubly-linked list.
  if (timer.previous != nullptr) {
    timer.previous->next = timer.next;
  } else if (inactive == &timer) {
    inactive = timer.next;
  } else {
    return;
  }

  if (timer.next != nullptr) {
    timer.next->previous = timer.previous;
  }
  timer.previous = nullptr;
  timer.child = nullptr;
  timer.next = nullptr;
}

detail::timer_slot* detail::timer_state_data::heap_front() const noexcept {
  return heap;
}

async_io::time_point detail::timer_state_data::heap_deadline() const noexcept {
  return heap != nullptr ? heap->expiry : async_io::time_point::max();
}

void detail::timer_state_data::clear() noexcept {
  while (timer_slot* const timer = pop_heap()) {
    timer->context = nullptr;
    timer->submitted = {};
  }

  while (inactive != nullptr) {
    timer_slot* const timer = inactive;
    erase_inactive(*timer);
    timer->context = nullptr;
    timer->submitted = {};
  }

  ready = nullptr;
  timeout_fetching.store(false, std::memory_order_release);
}

detail::timer_slot* detail::timer_state_data::meld(
    detail::timer_slot* first, detail::timer_slot* second) noexcept {
  if (first == nullptr) {
    return second;
  }
  if (second == nullptr) {
    return first;
  }
  // Ensure first has the earlier expiry (min-heap invariant).
  if (timer_less(*second, *first)) {
    std::swap(first, second);
  }

  // Link second as the first child of first (pairing-heap link).
  second->previous = first;
  second->next = first->child;
  if (first->child != nullptr) {
    first->child->previous = second;
  }
  first->child = second;
  return first;
}

detail::timer_slot* detail::timer_state_data::merge_pairs(
    detail::timer_slot* first) noexcept {
  // Pairwise merge pass: traverse the sibling list, melding adjacent
  // siblings and accumulating results into a single heap. Standard
  // two-pass pairing-heap merge-pairs.
  timer_slot* result = nullptr;
  while (first != nullptr) {
    timer_slot* const second = first->next;
    timer_slot* const remaining = second != nullptr ? second->next : nullptr;

    first->previous = nullptr;
    first->next = nullptr;
    if (second != nullptr) {
      second->previous = nullptr;
      second->next = nullptr;
    }

    result = meld(result, meld(first, second));
    first = remaining;
  }
  return result;
}

bool detail::timer_state_data::timer_less(
    const detail::timer_slot& first,
    const detail::timer_slot& second) noexcept {
  return first.expiry < second.expiry;
}

}  // namespace bnio
