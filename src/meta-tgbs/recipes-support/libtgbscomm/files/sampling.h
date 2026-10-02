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

/* API reserved for the sampling backend. For a valid sampling contract open
 * currently returns -1/ENOTSUP and leaves *channel NULL. Using the queuing API
 * on a sampling contract (or vice versa) fails with EPROTOTYPE.
 * All functions returning int use 0 for success and -1 with errno on failure. */
int tgbs_sampling_channel_open(const char *name,
	tgbs_channel_direction_t direction, tgbs_sampling_channel_t **channel);

/* Publish a complete nonempty message, replacing the previous value. */
int tgbs_sampling_channel_write(tgbs_sampling_channel_t *channel,
	const void *message, size_t length);

/* Read without consuming. The future backend will return the last value and
 * its freshness, using the contract's refresh_period_us and monotonic time.
 * An empty port returns -1/ENODATA and INVALID. A too-small buffer returns
 * -1/EMSGSIZE and the required length, without changing the stored value.
 * write/read currently return ENOTSUP for otherwise valid arguments. */
int tgbs_sampling_channel_read(tgbs_sampling_channel_t *channel,
	void *message, size_t capacity, size_t *length,
	tgbs_sampling_validity_t *validity);

/* Close the local reference; closing a reader must not remove the last value. */
void tgbs_sampling_channel_close(tgbs_sampling_channel_t *channel);

#ifdef __cplusplus
}
#endif
#endif
