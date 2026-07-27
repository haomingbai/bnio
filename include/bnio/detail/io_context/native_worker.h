/**
 * @file native_worker.h
 * @brief (removed — io_context no longer tracks native workers)
 *
 * This header was part of the old design where io_context maintained a
 * lock-free linked list of per-thread native_worker nodes for wakeup
 * and stop traversal.  That coupling introduced use-after-free risks
 * and has been replaced by base::wake_channel, a shared wake channel
 * owned by io_context (see native_task_queue_state::wake_channel_).
 *
 * The file is kept as a build-artifact placeholder; it is no longer
 * included by any io_context header.
 */

#ifndef BNIO_DETAIL_IO_CONTEXT_NATIVE_WORKER_H_
#define BNIO_DETAIL_IO_CONTEXT_NATIVE_WORKER_H_
#endif  // BNIO_DETAIL_IO_CONTEXT_NATIVE_WORKER_H_
