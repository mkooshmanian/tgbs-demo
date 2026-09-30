/*
 * SPDX-License-Identifier: MIT
 *
 * Public communication-channel API for TGBS domains.
 */

#ifndef TGBS_CHANNEL_H
#define TGBS_CHANNEL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tgbs_channel tgbs_channel_t;

typedef enum {
	TGBS_CHANNEL_SOURCE = 1,
	TGBS_CHANNEL_DESTINATION = 2
} tgbs_channel_direction_t;

typedef struct {
	tgbs_channel_direction_t direction;
	size_t max_message_size;
	int message_pending;
	size_t next_message_size;
} tgbs_channel_status_t;

/*
 * Open NAME in the requested direction.
 *
 * The immutable contract is read from /run/tgbs/channels/NAME/contract. A
 * source exclusively owns an unnamed AF_UNIX datagram socket. A destination
 * exclusively binds the named channel socket.
 *
 * Returns 0 on success and -1 with errno set on failure.
 */
int tgbs_channel_open(const char *name, tgbs_channel_direction_t direction,
		tgbs_channel_t **channel);

/* Send one complete nonempty message. */
int tgbs_channel_send(tgbs_channel_t *channel, const void *message,
		size_t length);

/*
 * Receive one complete message.
 *
 * CAPACITY is the size of MESSAGE. On success, LENGTH receives the number of
 * copied bytes. If the next message exceeds CAPACITY it is consumed, LENGTH
 * receives its original size, and the call fails with errno == EMSGSIZE.
 */
int tgbs_channel_receive(tgbs_channel_t *channel, void *message,
		size_t capacity, size_t *length);

/* Return the immutable configured maximum message size. */
size_t tgbs_channel_max_message_size(const tgbs_channel_t *channel);

/* Return the underlying descriptor for poll/select/epoll integration. */
int tgbs_channel_fd(const tgbs_channel_t *channel);

/* Query the contract and, for a destination, the next queued message. */
int tgbs_channel_get_status(tgbs_channel_t *channel,
		tgbs_channel_status_t *status);

void tgbs_channel_close(tgbs_channel_t *channel);

#ifdef __cplusplus
}
#endif

#endif
