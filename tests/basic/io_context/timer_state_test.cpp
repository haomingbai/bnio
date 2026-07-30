#include <bnio/detail/posix/io_context/timer_types.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <random>
#include <vector>

namespace {

void expect_active_heap(const bnio::detail::timer_slot* timer,
                        const bnio::detail::timer_slot* parent = nullptr) {
  const bnio::detail::timer_slot* previous = parent;
  for (const bnio::detail::timer_slot* current = timer; current != nullptr;
       current = current->next) {
    ASSERT_TRUE(current->active);
    EXPECT_EQ(current->previous, previous);
    if (parent != nullptr) {
      EXPECT_LE(parent->expiry, current->expiry);
    }
    expect_active_heap(current->child, current);
    previous = current;
  }
}

void expect_inactive_list(const bnio::detail::timer_state_data& timers) {
  const bnio::detail::timer_slot* previous = nullptr;
  for (const bnio::detail::timer_slot* current = timers.inactive;
       current != nullptr; current = current->next) {
    EXPECT_FALSE(current->active);
    EXPECT_EQ(current->previous, previous);
    EXPECT_EQ(current->child, nullptr);
    previous = current;
  }
}

TEST(TimerStateTest, linked_pairing_heap_removes_and_reinserts_exact_slot) {
  bnio::detail::timer_slot first;
  bnio::detail::timer_slot second;
  bnio::detail::timer_slot third;
  bnio::detail::timer_state_data timers;

  const auto now = bnio::async_io::clock::now();
  first.expiry = now + std::chrono::milliseconds(30);
  second.expiry = now + std::chrono::milliseconds(10);
  third.expiry = now + std::chrono::milliseconds(20);

  timers.push_heap(first);
  timers.push_heap(second);
  timers.push_heap(third);
  EXPECT_EQ(timers.heap_front(), &second);
  expect_active_heap(timers.heap);

  timers.erase_heap(second);
  EXPECT_FALSE(second.active);
  EXPECT_EQ(second.previous, nullptr);
  EXPECT_EQ(second.child, nullptr);
  EXPECT_EQ(second.next, nullptr);
  EXPECT_EQ(timers.heap_front(), &third);
  expect_active_heap(timers.heap);

  second.expiry = now + std::chrono::milliseconds(5);
  timers.push_heap(second);
  EXPECT_EQ(timers.heap_front(), &second);
  expect_active_heap(timers.heap);

  const std::array<bnio::detail::timer_slot*, 3> expected{
      &second,
      &third,
      &first,
  };
  for (bnio::detail::timer_slot* expected_timer : expected) {
    bnio::detail::timer_slot* const popped = timers.pop_heap();
    ASSERT_EQ(popped, expected_timer);
    EXPECT_FALSE(popped->active);
    EXPECT_EQ(popped->previous, nullptr);
    EXPECT_EQ(popped->child, nullptr);
    EXPECT_EQ(popped->next, nullptr);
    expect_active_heap(timers.heap);
  }
  EXPECT_EQ(timers.heap, nullptr);
}

TEST(TimerStateTest, inactive_list_unlinks_before_rearming_into_heap) {
  bnio::detail::timer_slot first;
  bnio::detail::timer_slot second;
  bnio::detail::timer_slot third;
  bnio::detail::timer_state_data timers;

  timers.push_inactive(first);
  timers.push_inactive(second);
  timers.push_inactive(third);
  ASSERT_EQ(timers.inactive, &third);
  expect_inactive_list(timers);

  timers.erase_inactive(second);
  EXPECT_EQ(second.previous, nullptr);
  EXPECT_EQ(second.child, nullptr);
  EXPECT_EQ(second.next, nullptr);
  EXPECT_EQ(timers.inactive, &third);
  EXPECT_EQ(third.next, &first);
  EXPECT_EQ(first.previous, &third);
  expect_inactive_list(timers);

  second.expiry = bnio::async_io::clock::now() + std::chrono::seconds(1);
  timers.push_heap(second);
  EXPECT_TRUE(second.active);
  EXPECT_EQ(timers.heap_front(), &second);
  EXPECT_EQ(timers.inactive, &third);
  expect_active_heap(timers.heap);
  expect_inactive_list(timers);
}

TEST(TimerStateTest, linked_pairing_heap_keeps_minimum_after_many_rekeys) {
  constexpr std::size_t kTimerCount = 64;
  constexpr std::size_t kRekeys = 512;
  std::array<bnio::detail::timer_slot, kTimerCount> timer_nodes;
  bnio::detail::timer_state_data timers;
  const auto now = bnio::async_io::clock::now();

  for (std::size_t index = 0; index < timer_nodes.size(); ++index) {
    timer_nodes[index].expiry =
        now + std::chrono::milliseconds(static_cast<int>(index * 7));
    timers.push_heap(timer_nodes[index]);
  }

  for (std::size_t round = 0; round < kRekeys; ++round) {
    const std::size_t index = (round * 17) % timer_nodes.size();
    timers.erase_heap(timer_nodes[index]);
    timer_nodes[index].expiry =
        now + std::chrono::milliseconds(static_cast<int>((round * 29) % 997));
    timers.push_heap(timer_nodes[index]);

    const auto minimum = timer_nodes[0].expiry;
    auto expected_minimum = minimum;
    for (const bnio::detail::timer_slot& slot : timer_nodes) {
      if (slot.expiry < expected_minimum) {
        expected_minimum = slot.expiry;
      }
    }
    ASSERT_NE(timers.heap_front(), nullptr);
    EXPECT_EQ(timers.heap_front()->expiry, expected_minimum);
    expect_active_heap(timers.heap);
  }
}

TEST(TimerStateTest, clear_invalidates_active_and_inactive_slots) {
  bnio::detail::timer_slot active;
  bnio::detail::timer_slot inactive;
  bnio::detail::timer_state_data timers;

  active.expiry = bnio::async_io::clock::now() + std::chrono::seconds(1);
  timers.push_heap(active);
  timers.push_inactive(inactive);
  timers.clear();

  EXPECT_EQ(timers.heap, nullptr);
  EXPECT_EQ(timers.inactive, nullptr);
  EXPECT_FALSE(active.active);
  EXPECT_FALSE(inactive.active);
  EXPECT_EQ(active.previous, nullptr);
  EXPECT_EQ(active.child, nullptr);
  EXPECT_EQ(active.next, nullptr);
  EXPECT_EQ(inactive.previous, nullptr);
  EXPECT_EQ(inactive.child, nullptr);
  EXPECT_EQ(inactive.next, nullptr);
}

TEST(TimerStateTest, empty_heap_pop_returns_null) {
  bnio::detail::timer_state_data timers;
  EXPECT_EQ(timers.pop_heap(), nullptr);
  EXPECT_EQ(timers.heap, nullptr);
}

TEST(TimerStateTest, single_node_round_trip) {
  bnio::detail::timer_slot node;
  bnio::detail::timer_state_data timers;

  node.expiry = bnio::async_io::clock::now() + std::chrono::seconds(1);
  timers.push_heap(node);
  EXPECT_TRUE(node.active);
  EXPECT_EQ(timers.heap_front(), &node);
  expect_active_heap(timers.heap);

  bnio::detail::timer_slot* const popped = timers.pop_heap();
  ASSERT_EQ(popped, &node);
  EXPECT_FALSE(node.active);
  EXPECT_EQ(node.previous, nullptr);
  EXPECT_EQ(node.child, nullptr);
  EXPECT_EQ(node.next, nullptr);
  EXPECT_EQ(timers.heap, nullptr);
  EXPECT_EQ(timers.heap_front(), nullptr);
}

TEST(TimerStateTest, erase_non_active_slot_returns_silently) {
  bnio::detail::timer_slot node;
  bnio::detail::timer_state_data timers;

  timers.erase_heap(node);
  EXPECT_FALSE(node.active);
  EXPECT_EQ(node.previous, nullptr);
  EXPECT_EQ(node.child, nullptr);
  EXPECT_EQ(node.next, nullptr);
  EXPECT_EQ(timers.heap, nullptr);
}

TEST(TimerStateTest, push_already_active_slot_returns_silently) {
  bnio::detail::timer_slot node;
  bnio::detail::timer_state_data timers;

  node.expiry = bnio::async_io::clock::now() + std::chrono::seconds(1);
  timers.push_heap(node);
  EXPECT_TRUE(node.active);

  bnio::detail::timer_slot* const heap_before = timers.heap;
  timers.push_heap(node);
  EXPECT_EQ(timers.heap, heap_before);
  expect_active_heap(timers.heap);
}

TEST(TimerStateTest, large_scale_random_rekeys) {
  constexpr std::size_t kTimerCount = 1000;
  constexpr std::size_t kRekeys = 2000;
  std::vector<bnio::detail::timer_slot> timer_nodes(kTimerCount);
  bnio::detail::timer_state_data timers;
  const auto now = bnio::async_io::clock::now();

  std::mt19937 rng(42);
  std::uniform_int_distribution<std::size_t> index_dist(0, kTimerCount - 1);
  std::uniform_int_distribution<int> expiry_dist(0, 10000);

  for (std::size_t i = 0; i < timer_nodes.size(); ++i) {
    timer_nodes[i].expiry =
        now + std::chrono::milliseconds(static_cast<int>(i * 3));
    timers.push_heap(timer_nodes[i]);
  }
  expect_active_heap(timers.heap);

  for (std::size_t round = 0; round < kRekeys; ++round) {
    const std::size_t index = index_dist(rng);
    timers.erase_heap(timer_nodes[index]);
    timer_nodes[index].expiry =
        now + std::chrono::milliseconds(expiry_dist(rng));
    timers.push_heap(timer_nodes[index]);
    expect_active_heap(timers.heap);
  }
}

TEST(TimerStateTest, erase_deep_child_restores_invariant) {
  constexpr std::size_t kCount = 20;
  std::array<bnio::detail::timer_slot, kCount> nodes;
  bnio::detail::timer_state_data timers;
  const auto now = bnio::async_io::clock::now();

  for (std::size_t i = 0; i < kCount; ++i) {
    nodes[i].expiry = now + std::chrono::milliseconds(static_cast<int>(i) * 10);
    timers.push_heap(nodes[i]);
  }
  expect_active_heap(timers.heap);

  timers.erase_heap(nodes[kCount - 1]);
  EXPECT_FALSE(nodes[kCount - 1].active);
  EXPECT_EQ(nodes[kCount - 1].previous, nullptr);
  EXPECT_EQ(nodes[kCount - 1].child, nullptr);
  EXPECT_EQ(nodes[kCount - 1].next, nullptr);
  expect_active_heap(timers.heap);

  EXPECT_EQ(timers.heap_front(), &nodes[0]);
}

TEST(TimerStateTest, erase_inactive_head_and_tail) {
  bnio::detail::timer_slot first;
  bnio::detail::timer_slot second;
  bnio::detail::timer_slot third;
  bnio::detail::timer_state_data timers;

  timers.push_inactive(first);
  timers.push_inactive(second);
  timers.push_inactive(third);
  ASSERT_EQ(timers.inactive, &third);
  expect_inactive_list(timers);

  timers.erase_inactive(third);
  EXPECT_EQ(timers.inactive, &second);
  EXPECT_EQ(second.previous, nullptr);
  EXPECT_EQ(second.next, &first);
  EXPECT_EQ(first.previous, &second);
  EXPECT_EQ(first.next, nullptr);
  expect_inactive_list(timers);

  timers.erase_inactive(first);
  EXPECT_EQ(timers.inactive, &second);
  EXPECT_EQ(second.previous, nullptr);
  EXPECT_EQ(second.next, nullptr);
  expect_inactive_list(timers);
}

TEST(TimerStateTest, meld_equal_expiry_maintains_structure) {
  constexpr std::size_t kCount = 10;
  std::array<bnio::detail::timer_slot, kCount> nodes;
  bnio::detail::timer_state_data timers;
  const auto now = bnio::async_io::clock::now();

  for (std::size_t i = 0; i < kCount; ++i) {
    nodes[i].expiry = now + std::chrono::seconds(1);
    timers.push_heap(nodes[i]);
  }
  expect_active_heap(timers.heap);

  std::size_t popped_count = 0;
  while (timers.heap_front() != nullptr) {
    bnio::detail::timer_slot* const popped = timers.pop_heap();
    ASSERT_NE(popped, nullptr);
    EXPECT_FALSE(popped->active);
    ++popped_count;
    expect_active_heap(timers.heap);
  }
  EXPECT_EQ(popped_count, kCount);
  EXPECT_EQ(timers.heap, nullptr);
}

TEST(TimerStateTest, heap_front_returns_null_for_empty) {
  bnio::detail::timer_state_data timers;
  EXPECT_EQ(timers.heap_front(), nullptr);
}

TEST(TimerStateTest, heap_deadline_returns_max_for_empty) {
  bnio::detail::timer_state_data timers;
  EXPECT_EQ(timers.heap_deadline(), bnio::async_io::time_point::max());
}

}  // namespace
