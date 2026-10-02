/* SPDX-License-Identifier: MIT
 * Sampling contract/API scaffolding; publication is implemented separately. */
#include "sampling.h"
#include "comm-internal.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

int tgbs_sampling_channel_open(const char *name,
	tgbs_channel_direction_t direction, tgbs_sampling_channel_t **channel)
{
	struct tgbs_channel_contract contract;
	int fd;

	if (channel == NULL) {
		errno = EINVAL;
		return -1;
	}
	*channel = NULL;
	fd = tgbs_comm_open_contract(name, direction, TGBS_CONTRACT_SAMPLING, &contract);
	if (fd < 0)
		return -1;
	close(fd);
	errno = ENOTSUP;
	return -1;
}

int tgbs_sampling_channel_write(tgbs_sampling_channel_t *channel,
	const void *message, size_t length)
{
	errno = channel == NULL || message == NULL || length == 0 ? EINVAL : ENOTSUP;
	return -1;
}

int tgbs_sampling_channel_read(tgbs_sampling_channel_t *channel,
	void *message, size_t capacity, size_t *length,
	tgbs_sampling_validity_t *validity)
{
	if (length != NULL)
		*length = 0;
	if (validity != NULL)
		*validity = TGBS_SAMPLING_INVALID;
	errno = channel == NULL || length == NULL || validity == NULL ||
		(message == NULL && capacity != 0) ? EINVAL : ENOTSUP;
	return -1;
}

void tgbs_sampling_channel_close(tgbs_sampling_channel_t *channel)
{
	free(channel);
}
