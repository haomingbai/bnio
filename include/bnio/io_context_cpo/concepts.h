/**
 * @file concepts.h
 * @brief I/O customization point concepts.
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_CPO_CONCEPTS_H_
#define BNIO_IO_CONTEXT_CPO_CONCEPTS_H_

#include <bnio/async_io/dns.h>
#include <bnio/io_context_cpo/instances.h>

#include <bexec/scheduler.hpp>
#include <bexec/sender.hpp>
#include <type_traits>
#include <utility>

namespace bnio {

/**
 * Concept satisfied when a scheduler returns a sender from async_read.
 */
template <class Scheduler, class Source, class Buffer>
concept reads_bytes =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Source& source, Buffer&& buffer) {
      {
        async_read(scheduler, source, std::forward<Buffer>(buffer))
      } -> bexec::sender;
    };

/**
 * Concept satisfied when a scheduler returns a sender from async_write.
 */
template <class Scheduler, class Sink, class Buffer>
concept writes_bytes =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Sink& sink, Buffer&& buffer) {
      {
        async_write(scheduler, sink, std::forward<Buffer>(buffer))
      } -> bexec::sender;
    };

/**
 * Concept satisfied when a provider returns a sender from async_accept.
 */
template <class Scheduler, class Acceptor>
concept accepts_connections =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Acceptor& acceptor) {
      { async_accept(scheduler, acceptor) } -> bexec::sender;
    };

/**
 * Concept satisfied when a scheduler returns a sender from async_connect.
 */
template <class Scheduler, class Stream, class Endpoint>
concept connects_stream =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Stream& stream, Endpoint&& endpoint) {
      {
        async_connect(scheduler, stream, std::forward<Endpoint>(endpoint))
      } -> bexec::sender;
    };

/**
 * Concept satisfied when a scheduler returns a sender from async_poll.
 */
template <class Scheduler, class Descriptor>
concept polls_descriptor =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Descriptor& descriptor, unsigned poll_mask) {
      { async_poll(scheduler, descriptor, poll_mask) } -> bexec::sender;
    };

/**
 * Concept satisfied when a scheduler returns a sender from async_resolve.
 */
template <class Scheduler, class Query>
concept resolves_dns =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Query&& query,
             async_io::dns_result_view result) {
      {
        async_resolve(scheduler, std::forward<Query>(query), result)
      } -> bexec::sender;
    };

}  // namespace bnio

#endif  // BNIO_IO_CONTEXT_CPO_CONCEPTS_H_
