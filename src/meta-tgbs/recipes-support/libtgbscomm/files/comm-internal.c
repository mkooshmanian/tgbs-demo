/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "comm-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

int tgbs_comm_make_path(char *out, size_t capacity, const char *name,
	const char *suffix)
{
	int n = snprintf(out, capacity, "%s/%s%s", TGBS_CHANNEL_ROOT, name, suffix);

	if (n < 0 || (size_t)n >= capacity) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

int tgbs_comm_open_contract(const char *name, tgbs_channel_direction_t direction,
	enum tgbs_contract_type type, struct tgbs_channel_contract *contract)
{
	char path[PATH_MAX];
	int fd;
	int saved_errno;

	if (!tgbs_contract_valid_name(name, TGBS_CONTRACT_NAME_SIZE) ||
	    (direction != TGBS_CHANNEL_SOURCE && direction != TGBS_CHANNEL_DESTINATION)) {
		errno = EINVAL;
		return -1;
	}
	if (tgbs_comm_make_path(path, sizeof(path), name, "/lifetime.lock") != 0)
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (flock(fd, LOCK_SH) != 0 ||
	    tgbs_contract_read(TGBS_CHANNEL_ROOT, name, contract) != 0)
		goto error;
	if (contract->type != type) {
		errno = EPROTOTYPE;
		goto error;
	}
	return fd;
error:
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return -1;
}
