/*
 * SPDX-License-Identifier: MIT
 *
 * Public queuing-channel API for TGBS domains.
 */

#ifndef TGBS_QUEUING_CHANNEL_H
#define TGBS_QUEUING_CHANNEL_H

#include <stddef.h>
#include <stdint.h>
#include "types.h"

#define TGBS_TIMEOUT_INFINITE INT64_C(-1)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tgbs_queuing_channel tgbs_queuing_channel_t;

typedef struct {
	tgbs_channel_direction_t direction;
	size_t max_message_size;
	int message_pending;
	size_t next_message_size;
} tgbs_queuing_channel_status_t;

/*
 * Open NAME in the requested direction.
 *
 * The immutable contract is read from /run/tgbs/channels/NAME/contract. A
 * source exclusively owns an unnamed AF_UNIX datagram socket. A destination
 * exclusively binds the named channel socket.
 * Open only prepares the local endpoint; the source may open before the
 * destination. The channel contract must already exist in either case.
 *
 * Returns 0 on success and -1 with errno set on failure.
 * A sampling contract is rejected with EPROTOTYPE.
 */
int tgbs_queuing_channel_open(const char *name, tgbs_channel_direction_t direction,
		tgbs_queuing_channel_t **channel);

/*
 * Send one complete nonempty message.
 *
 * TIMEOUT_US applies to both send and receive:
 *   0: do not wait; unavailable space/data returns EAGAIN.
 *   >0: wait passively for at most this many microseconds; expiration returns
 *       ETIMEDOUT. Linux timer granularity and scheduling may delay return.
 *   TGBS_TIMEOUT_INFINITE: wait passively without a time limit.
 * Other negative values or durations not representable by the wait timeout
 * return EINVAL. Signals are retried without restarting a finite deadline.
 * Send associates the socket with the destination at each I/O attempt. An
 * absent, closed or disconnected destination returns ENOTCONN immediately,
 * even for an infinite timeout; no message is buffered for later delivery.
 * Other errors (including permission/resource failures) are preserved.
 * Retrying send after the destination is recreated does not require reopening
 * the source. A pending send may also select the new destination on retry.
 *
 * Concurrent calls on a shared handle (including after fork) have independent
 * deadlines and compete for messages/space, with no fairness guarantee. I/O
 * uses MSG_DONTWAIT followed by passive ppoll waiting, without changing socket
 * timeout options or O_NONBLOCK. Callers must not close the handle or reconnect
 * or shut down its socket while an operation is in progress.
 */
int tgbs_queuing_channel_send(tgbs_queuing_channel_t *channel, const void *message,
		size_t length, int64_t timeout_us);

/*
 * Receive one complete message.
 *
 * CAPACITY is the size of MESSAGE. On success, LENGTH receives the number of
 * copied bytes. If the next message exceeds CAPACITY it is consumed, LENGTH
 * receives its original size, and the call fails with errno == EMSGSIZE.
 * On other failures, LENGTH is zero if it is not NULL. TIMEOUT_US follows the
 * rules documented for send.
 */
int tgbs_queuing_channel_receive(tgbs_queuing_channel_t *channel, void *message,
		size_t capacity, size_t *length, int64_t timeout_us);

/* Return the immutable configured maximum message size. */
size_t tgbs_queuing_channel_max_message_size(const tgbs_queuing_channel_t *channel);

/* Return the underlying descriptor for poll/select/epoll integration.
 * Source POLLOUT only accounts for the currently associated peer, if any;
 * it is not a guarantee that the channel's destination is available. */
int tgbs_queuing_channel_fd(const tgbs_queuing_channel_t *channel);

/* Query the contract and, for a destination, the next queued message.
 * This is a snapshot, not a reservation against concurrent receivers. */
int tgbs_queuing_channel_get_status(tgbs_queuing_channel_t *channel,
		tgbs_queuing_channel_status_t *status);

void tgbs_queuing_channel_close(tgbs_queuing_channel_t *channel);

#ifdef __cplusplus
}
#endif

#endif
