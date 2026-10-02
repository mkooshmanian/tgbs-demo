/* SPDX-License-Identifier: MIT */
#ifndef TGBS_SAMPLING_CHANNEL_H
#define TGBS_SAMPLING_CHANNEL_H

#include <stddef.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tgbs_sampling_channel tgbs_sampling_channel_t;

typedef enum {
	TGBS_SAMPLING_INVALID = 0,
	TGBS_SAMPLING_VALID = 1
} tgbs_sampling_validity_t;

/* Open a sampling channel in the requested direction. Multiple processes may
 * open the same role independently. The runtime mounts grant write access
 * only to the source partition. A queuing contract fails with EPROTOTYPE.
 * Every handle holds lifetime.lock; a failed open leaves *channel NULL.
 * All functions returning int use 0 for success and -1 with errno on failure. */
int tgbs_sampling_channel_open(const char *name,
	tgbs_channel_direction_t direction, tgbs_sampling_channel_t **channel);

/* Publish a complete nonempty message, replacing the previous value atomically.
 * Calls from multiple source processes/threads are serialized. Exceeding the
 * contract's maximum message size returns EMSGSIZE without replacing the value.
 * Failures before publication leave the previous value intact. */
int tgbs_sampling_channel_write(tgbs_sampling_channel_t *channel,
	const void *message, size_t length);

/* Read without consuming. Return the last complete message and its freshness,
 * using the contract's refresh_period_us and CLOCK_MONOTONIC. The message is
 * VALID only when its age is strictly less than the refresh period. An expired
 * message remains readable: the call succeeds with INVALID.
 * An empty port returns -1/ENODATA and INVALID. A too-small buffer returns
 * -1/EMSGSIZE and the required length, without changing the stored value.
 * Malformed samples return EPROTO. On failure, validity is INVALID.
 * Calling read on a source or write on a destination returns EINVAL. */
int tgbs_sampling_channel_read(tgbs_sampling_channel_t *channel,
	void *message, size_t capacity, size_t *length,
	tgbs_sampling_validity_t *validity);

/* Close the local reference. The last value persists after all handles close. */
void tgbs_sampling_channel_close(tgbs_sampling_channel_t *channel);

#ifdef __cplusplus
}
#endif
#endif
