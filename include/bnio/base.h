/**
 * @file base.h
 * @brief Aggregate header for the bnio::base layer.
 */

#pragma once
#ifndef BNIO_BASE_H_
#define BNIO_BASE_H_

#include <bnio/base/config.h>

// The base layer wraps Linux io_uring types; excluded when that backend is off.
#if defined(BNIO_HAS_BASE_LINUX)
#include <bnio/base/linux/completion_queue_entry.h>
#include <bnio/base/linux/params.h>
#include <bnio/base/linux/probe.h>
#include <bnio/base/linux/ring.h>
#include <bnio/base/linux/submission_queue_entry.h>
#endif

// The BSD base layer wraps kqueue/kevent types.
#if defined(BNIO_HAS_BASE_BSD)
#include <bnio/base/bsd/event.h>
#include <bnio/base/bsd/event_list_view.h>
#include <bnio/base/bsd/kqueue.h>
#endif

#endif  // BNIO_BASE_H_
