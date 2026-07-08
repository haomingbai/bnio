#pragma once
#ifndef BUPP_BASE_H_
#define BUPP_BASE_H_

#include <bupp/base/config.h>

// The base layer wraps Linux io_uring types; excluded when that backend is off.
#if defined(BUPP_HAS_BASE_LINUX)
#include <bupp/base/linux/completion_queue_entry.h>
#include <bupp/base/linux/params.h>
#include <bupp/base/linux/probe.h>
#include <bupp/base/linux/ring.h>
#include <bupp/base/linux/submission_queue_entry.h>
#endif

// The BSD base layer wraps kqueue/kevent types.
#if defined(BUPP_HAS_BASE_BSD)
#include <bupp/base/bsd/event.h>
#include <bupp/base/bsd/event_list_view.h>
#include <bupp/base/bsd/kqueue.h>
#endif

#endif  // BUPP_BASE_H_
