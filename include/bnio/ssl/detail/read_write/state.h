/**
 * @file state.h
 * @brief SSL read/write operation state machine states.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_READ_WRITE_STATE_H_
#define BNIO_SSL_DETAIL_READ_WRITE_STATE_H_

#include <bnio/ssl/detail/common.h>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

enum class application_io {
  read,
  write,
};

enum class io_phase {
  application,
  flush_output,
  transport_read,
  transport_write,
  done,
};

template <class Scheduler, class NextLayer, class Holder, class Receiver,
          application_io Application, bool CompleteBuffer>
class io_operation;

template <class State, class Receiver>
class step_operation;

template <class Scheduler, class NextLayer, class Holder,
          application_io Application, bool CompleteBuffer>
struct io_state {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using next_layer_type = NextLayer;
  static constexpr application_io application = Application;
  static constexpr bool complete_buffer = CompleteBuffer;
  static constexpr resume_action application_action =
      Application == application_io::read
          ? resume_action::application_read
          : resume_action::application_write;

  io_state(scheduler_type scheduler, tcp::stream<NextLayer>& stream,
           Holder buffer)
      : scheduler(std::move(scheduler)),
        stream(&stream),
        buffer(std::move(buffer)) {}

  scheduler_type scheduler;
  tcp::stream<NextLayer>* stream;
  Holder buffer;
  char* transport_data = nullptr;
  std::size_t transport_size = 0;
  std::size_t bytes = 0;
  io_phase phase = io_phase::application;
  resume_action after_flush = application_action;
  bool done = false;
};

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_READ_WRITE_STATE_H_
