/**
 * @file stop_transition.h
 * @brief Two-phase stop transition state shared by the native backends.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_STOP_TRANSITION_H_
#define BNIO_ASYNC_IO_STOP_TRANSITION_H_

namespace bnio::async_io {

/**
 * State stored in a task queue's life_state int.
 *
 * none accepts new work and means no stop transition has started;
 * aborting is the transient Phase-2 state in which the elected caller
 * aborts pending timer waits; stopping is terminal: no new work is
 * accepted and workers may finish.
 */
enum class stop_transition : int {
  none = 0,
  aborting = 1,
  stopping = 2,
};

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_STOP_TRANSITION_H_
