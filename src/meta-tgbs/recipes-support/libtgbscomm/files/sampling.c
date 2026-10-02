/* SPDX-License-Identifier: MIT
 * Sampling channels backed by atomically replaced timestamp/message files. */
#define _GNU_SOURCE
#include "sampling.h"
#include "comm-internal.h"
#include "sampling-format.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SOURCE_LOCK "source.lock"

struct tgbs_sampling_channel {
	int lifetime_fd;
	int endpoint_fd;
	tgbs_channel_direction_t direction;
	size_t max_message_size;
	uint64_t refresh_period_ns;
};

static int monotonic_ns(uint64_t *now)
{
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
		return -1;
	if (time.tv_sec < 0 ||
	    (uint64_t)time.tv_sec > (UINT64_MAX - (uint64_t)time.tv_nsec) / 1000000000) {
		errno = EOVERFLOW;
		return -1;
	}
	*now = (uint64_t)time.tv_sec * 1000000000 + (uint64_t)time.tv_nsec;
	return 0;
}

static int write_all(int fd, const void *data, size_t length, off_t offset)
{
	const unsigned char *bytes = data;

	while (length > 0) {
		ssize_t written = pwrite(fd, bytes, length, offset);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (written == 0) {
			errno = EIO;
			return -1;
		}
		bytes += written;
		length -= (size_t)written;
		offset += written;
	}
	return 0;
}

static int read_all(int fd, void *data, size_t length, off_t offset)
{
	unsigned char *bytes = data;

	while (length > 0) {
		ssize_t received = pread(fd, bytes, length, offset);

		if (received < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (received == 0) {
			errno = EPROTO;
			return -1;
		}
		bytes += received;
		length -= (size_t)received;
		offset += received;
	}
	return 0;
}

static int create_temporary(int endpoint_fd, char *name, size_t capacity)
{
	for (unsigned int attempt = 0; attempt < 16; attempt++) {
		uint64_t nonce;
		unsigned char *bytes = (unsigned char *)&nonce;
		size_t remaining = sizeof(nonce);
		int fd;

		while (remaining > 0) {
			ssize_t received = getrandom(bytes, remaining, 0);

			if (received < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (received == 0) {
				errno = EIO;
				return -1;
			}
			bytes += received;
			remaining -= (size_t)received;
		}
		snprintf(name, capacity, TGBS_SAMPLING_TEMP_PREFIX "%016llx",
			(unsigned long long)nonce);
		fd = openat(endpoint_fd, name,
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd >= 0)
			return fd;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}

int tgbs_sampling_channel_open(const char *name,
	tgbs_channel_direction_t direction, tgbs_sampling_channel_t **out)
{
	struct tgbs_channel_contract contract;
	struct tgbs_sampling_channel *channel;
	char path[PATH_MAX];
	int saved_errno;

	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	*out = NULL;
	channel = calloc(1, sizeof(*channel));
	if (channel == NULL)
		return -1;
	channel->lifetime_fd = -1;
	channel->endpoint_fd = -1;
	channel->direction = direction;
	channel->lifetime_fd = tgbs_comm_open_contract(name, direction,
		TGBS_CONTRACT_SAMPLING, &contract);
	if (channel->lifetime_fd < 0)
		goto error;
	channel->max_message_size = (size_t)contract.max_message_size;
	channel->refresh_period_ns = (uint64_t)contract.refresh_period_us * 1000;
	if (tgbs_comm_make_path(path, sizeof(path), name, "/endpoint") != 0)
		goto error;
	channel->endpoint_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (channel->endpoint_fd < 0)
		goto error;
	if (direction == TGBS_CHANNEL_SOURCE) {
		/* Only a source has a writable source.lock mount. No exclusive lock
		 * is held for the handle's lifetime: processes may open independently. */
		int fd = openat(channel->endpoint_fd, SOURCE_LOCK,
			O_RDWR | O_CLOEXEC | O_NOFOLLOW);

		if (fd < 0)
			goto error;
		if (close(fd) != 0)
			goto error;
	}
	*out = channel;
	return 0;
error:
	saved_errno = errno;
	tgbs_sampling_channel_close(channel);
	errno = saved_errno;
	return -1;
}

int tgbs_sampling_channel_write(tgbs_sampling_channel_t *channel,
	const void *message, size_t length)
{
	char temporary[TGBS_SAMPLING_TEMP_NAME_SIZE];
	uint64_t timestamp;
	int lock_fd;
	int fd = -1;
	int saved_errno;
	int result;
	int have_temporary = 0;

	if (channel == NULL || channel->direction != TGBS_CHANNEL_SOURCE ||
	    message == NULL || length == 0) {
		errno = EINVAL;
		return -1;
	}
	if (length > channel->max_message_size) {
		errno = EMSGSIZE;
		return -1;
	}
	/* A fresh open file description for each operation also serializes
	 * threads or forked children sharing the same channel handle. */
	lock_fd = openat(channel->endpoint_fd, SOURCE_LOCK,
		O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (lock_fd < 0)
		return -1;
	do {
		result = flock(lock_fd, LOCK_EX);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		goto error;
	fd = create_temporary(channel->endpoint_fd, temporary, sizeof(temporary));
	if (fd < 0)
		goto error;
	have_temporary = 1;
	if (write_all(fd, message, length, TGBS_SAMPLING_TIMESTAMP_SIZE) != 0 ||
	    monotonic_ns(&timestamp) != 0)
		goto error;
	/* Record time after copying the payload, immediately before publication. */
	timestamp = htole64(timestamp);
	if (write_all(fd, &timestamp, sizeof(timestamp), 0) != 0 ||
	    fchmod(fd, 0444) != 0)
		goto error;
	result = close(fd);
	fd = -1;
	if (result != 0)
		goto error;
	if (renameat(channel->endpoint_fd, temporary,
			channel->endpoint_fd, TGBS_SAMPLING_FILE_NAME) != 0)
		goto error;
	close(lock_fd);
	return 0;
error:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	if (have_temporary)
		unlinkat(channel->endpoint_fd, temporary, 0);
	close(lock_fd);
	errno = saved_errno;
	return -1;
}

int tgbs_sampling_channel_read(tgbs_sampling_channel_t *channel,
	void *message, size_t capacity, size_t *length,
	tgbs_sampling_validity_t *validity)
{
	struct stat st;
	uint64_t timestamp, now;
	size_t message_size;
	int fd;
	int saved_errno;

	if (length != NULL)
		*length = 0;
	if (validity != NULL)
		*validity = TGBS_SAMPLING_INVALID;
	if (channel == NULL || channel->direction != TGBS_CHANNEL_DESTINATION ||
	    length == NULL || validity == NULL || (message == NULL && capacity != 0)) {
		errno = EINVAL;
		return -1;
	}
	/* Open on every read: a descriptor retained across rename would keep
	 * reading an older inode. Metadata and data use this same descriptor. */
	fd = openat(channel->endpoint_fd, TGBS_SAMPLING_FILE_NAME,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT)
			errno = ENODATA;
		return -1;
	}
	if (fstat(fd, &st) != 0)
		goto error;
	if (!S_ISREG(st.st_mode) || st.st_size <= (off_t)TGBS_SAMPLING_TIMESTAMP_SIZE ||
	    (uint64_t)st.st_size - TGBS_SAMPLING_TIMESTAMP_SIZE > channel->max_message_size) {
		errno = EPROTO;
		goto error;
	}
	message_size = (size_t)st.st_size - TGBS_SAMPLING_TIMESTAMP_SIZE;
	*length = message_size;
	if (message_size > capacity) {
		errno = EMSGSIZE;
		goto error;
	}
	if (read_all(fd, &timestamp, sizeof(timestamp), 0) != 0 ||
	    read_all(fd, message, message_size, TGBS_SAMPLING_TIMESTAMP_SIZE) != 0 ||
	    monotonic_ns(&now) != 0)
		goto error;
	timestamp = le64toh(timestamp);
	if (timestamp > now) {
		errno = EPROTO;
		goto error;
	}
	*validity = now - timestamp < channel->refresh_period_ns ?
		TGBS_SAMPLING_VALID : TGBS_SAMPLING_INVALID;
	close(fd);
	return 0;
error:
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return -1;
}

void tgbs_sampling_channel_close(tgbs_sampling_channel_t *channel)
{
	if (channel == NULL)
		return;
	if (channel->endpoint_fd >= 0)
		close(channel->endpoint_fd);
	if (channel->lifetime_fd >= 0)
		close(channel->lifetime_fd);
	free(channel);
}
