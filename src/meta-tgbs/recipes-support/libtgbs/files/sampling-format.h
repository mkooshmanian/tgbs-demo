/* SPDX-License-Identifier: MIT */
#ifndef TGBS_SAMPLING_FORMAT_H
#define TGBS_SAMPLING_FORMAT_H

#include <string.h>

/* A sample consists of an unsigned 64-bit little-endian CLOCK_MONOTONIC
 * timestamp in nanoseconds, followed by the nonempty message. Its length is
 * the file size minus this fixed header; there is no message queue. */
#define TGBS_SAMPLING_TIMESTAMP_SIZE 8U
#define TGBS_SAMPLING_FILE_NAME "sample"
#define TGBS_SAMPLING_FILE "/endpoint/" TGBS_SAMPLING_FILE_NAME
#define TGBS_SAMPLING_TEMP_PREFIX ".sample.tmp-"
#define TGBS_SAMPLING_TEMP_NAME_SIZE (sizeof(TGBS_SAMPLING_TEMP_PREFIX) + 16)

/* Reserve only the exact filenames emitted by the sampling writer. */
static inline int tgbs_sampling_is_temporary(const char *name)
{
	size_t prefix_length = sizeof(TGBS_SAMPLING_TEMP_PREFIX) - 1;
	size_t i;

	if (strlen(name) != prefix_length + 16 ||
	    strncmp(name, TGBS_SAMPLING_TEMP_PREFIX, prefix_length) != 0)
		return 0;
	for (i = prefix_length; i < prefix_length + 16; i++)
		if (!((name[i] >= '0' && name[i] <= '9') ||
		      (name[i] >= 'a' && name[i] <= 'f')))
			return 0;
	return 1;
}

#endif
