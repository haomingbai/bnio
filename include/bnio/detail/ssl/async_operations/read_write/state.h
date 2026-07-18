#pragma once
#ifndef BNIO_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_STATE_H_
#define BNIO_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_STATE_H_

#include <bnio/detail/ssl/async_operations/common.h>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace bnio {

/** @cond BNIO_DETAIL */
namespace detail {

enum class ssl_application_io {
  read,
  write,
};

enum class ssl_io_phase {
  application,
  flush_output,
  transport_read,
  transport_write,
  done,
};

template <class Scheduler, class NextLayer, class Holder, class Receiver,
          ssl_application_io Application, bool CompleteBuffer>
class ssl_io_operation;

template <class State, class Receiver>
class ssl_io_step_operation;

template <class Scheduler, class NextLayer, class Holder,
          ssl_application_io Application, bool CompleteBuffer>
struct ssl_io_state {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using next_layer_type = NextLayer;
  static constexpr ssl_application_io application = Application;
  static constexpr bool complete_buffer = CompleteBuffer;
  static constexpr ssl_resume_action application_action =
      Application == ssl_application_io::read
          ? ssl_resume_action::application_read
          : ssl_resume_action::application_write;

  ssl_io_state(scheduler_type scheduler, ssl_stream<NextLayer>& stream,
               Holder buffer)
      : scheduler(std::move(scheduler)),
        stream(&stream),
        buffer(std::move(buffer)) {}

  scheduler_type scheduler;
  ssl_stream<NextLayer>* stream;
  Holder buffer;
  char* transport_data = nullptr;
  std::size_t transport_size = 0;
  std::size_t bytes = 0;
  ssl_io_phase phase = ssl_io_phase::application;
  ssl_resume_action after_flush = application_action;
  bool done = false;
};

}  // namespace detail
/** @endcond */

}  // namespace bnio

#endif  // BNIO_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_STATE_H_
