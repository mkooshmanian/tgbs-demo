/*
 * SPDX-License-Identifier: MIT
 *
 * AF_UNIX SOCK_DGRAM backend for queuing channels.
 */
#define _GNU_SOURCE
#include "queuing.h"
#include "comm-internal.h"
#include "queuing-format.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOURCE_LOCK "/endpoint/source.lock"
#define DESTINATION_LOCK "/endpoint/destination.lock"

struct tgbs_queuing_channel {
	int fd;
	int lifetime_fd;
	int source_lock_fd;
	int destination_lock_fd;
	tgbs_channel_direction_t direction;
	size_t max_message_size;
	char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	dev_t socket_device;
	ino_t socket_inode;
};

struct queuing_timeout {
	int64_t duration_us;
	uint64_t deadline_us;
};

static int monotonic_us(uint64_t *now)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return -1;
	*now = (uint64_t)ts.tv_sec * UINT64_C(1000000) + ts.tv_nsec / 1000;
	return 0;
}

static int timeout_init(struct queuing_timeout *timeout, int64_t duration_us)
{
	uint64_t now;

	/* Bound the seconds field of ppoll's timespec on the 32-bit target. */
	if (duration_us < TGBS_TIMEOUT_INFINITE ||
	    duration_us / INT64_C(1000000) > LONG_MAX) {
		errno = EINVAL;
		return -1;
	}
	timeout->duration_us = duration_us;
	timeout->deadline_us = 0;
	if (duration_us > 0) {
		if (monotonic_us(&now) != 0)
			return -1;
		timeout->deadline_us = now + (uint64_t)duration_us;
	}
	return 0;
}

static int timeout_remaining(const struct queuing_timeout *timeout,
		struct timespec *remaining)
{
	uint64_t now, duration;

	if (timeout->duration_us <= 0)
		return 0;
	if (monotonic_us(&now) != 0)
		return -1;
	if (now >= timeout->deadline_us) {
		errno = ETIMEDOUT;
		return -1;
	}
	duration = timeout->deadline_us - now;
	remaining->tv_sec = (time_t)(duration / UINT64_C(1000000));
	remaining->tv_nsec = (long)(duration % UINT64_C(1000000)) * 1000;
	return 0;
}

static int wait_ready(int fd, short events, const struct queuing_timeout *timeout)
{
	struct pollfd poll_fd = { .fd = fd, .events = events };
	struct timespec remaining;
	int ready;

	if (timeout->duration_us == 0) {
		errno = EAGAIN;
		return -1;
	}
	for (;;) {
		if (timeout_remaining(timeout, &remaining) != 0)
			return -1;
		poll_fd.revents = 0;
		ready = ppoll(&poll_fd, 1,
			timeout->duration_us > 0 ? &remaining : NULL, NULL);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (ready == 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (poll_fd.revents & POLLNVAL) {
			errno = EBADF;
			return -1;
		}
		if (poll_fd.revents & POLLERR) {
			int socket_error;
			socklen_t size = sizeof(socket_error);

			if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
					&socket_error, &size) != 0)
				return -1;
			errno = socket_error != 0 ? socket_error : EIO;
			return -1;
		}
		if (poll_fd.revents & POLLHUP) {
			errno = EPIPE;
			return -1;
		}
		if (poll_fd.revents & events)
			return 0;
	}
}

static int configure_sender_buffer(int fd, size_t max_message_size)
{
	int requested = (int)max_message_size + 32;
	int actual;
	socklen_t actual_size = sizeof(actual);

	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
			&requested, sizeof(requested)) != 0)
		return -1;
	if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF,
			&actual, &actual_size) != 0)
		return -1;
	if (actual < 0 || (size_t)actual < max_message_size + 32) {
		errno = EMSGSIZE;
		return -1;
	}
	return 0;
}

static socklen_t unix_address(struct sockaddr_un *address, const char *path)
{
	size_t length = strlen(path);

	memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;
	memcpy(address->sun_path, path, length + 1);
	return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
}

static int open_endpoint_lock(const char *name, const char *suffix)
{
	char path[PATH_MAX];
	int fd;

	if (tgbs_comm_make_path(path, sizeof(path), name, suffix) != 0)
		return -1;
	fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		int saved_errno = errno == EWOULDBLOCK ? EADDRINUSE : errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}
	return fd;
}

static int open_source(struct tgbs_queuing_channel *channel, const char *name)
{
	channel->source_lock_fd = open_endpoint_lock(name, SOURCE_LOCK);
	if (channel->source_lock_fd < 0)
		return -1;
	channel->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (channel->fd < 0)
		return -1;
	return configure_sender_buffer(channel->fd,
		channel->max_message_size);
}

static int open_destination(struct tgbs_queuing_channel *channel, const char *name)
{
	struct sockaddr_un address;
	socklen_t address_length;
	struct stat socket_stat;

	channel->destination_lock_fd = open_endpoint_lock(name, DESTINATION_LOCK);
	if (channel->destination_lock_fd < 0)
		return -1;

	channel->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (channel->fd < 0)
		return -1;

	/* Holding destination.lock proves that a socket left at this pathname is
	 * stale. No other libtgbscomm destination can be bound concurrently. */
	struct stat old_socket;
	if (lstat(channel->socket_path, &old_socket) == 0) {
		if (!S_ISSOCK(old_socket.st_mode)) {
			errno = EEXIST;
			return -1;
		}
		if (unlink(channel->socket_path) != 0)
			return -1;
	} else if (errno != ENOENT) {
		return -1;
	}
	address_length = unix_address(&address, channel->socket_path);
	if (bind(channel->fd, (struct sockaddr *)&address,
			address_length) != 0)
		return -1;
	if (chmod(channel->socket_path, 0222) != 0)
		return -1;
	if (lstat(channel->socket_path, &socket_stat) != 0)
		return -1;
	channel->socket_device = socket_stat.st_dev;
	channel->socket_inode = socket_stat.st_ino;
	return 0;
}

int tgbs_queuing_channel_open(const char *name, tgbs_channel_direction_t direction,
		tgbs_queuing_channel_t **out)
{
	struct tgbs_channel_contract contract;
	struct tgbs_queuing_channel *channel;
	char socket_path[PATH_MAX];
	int saved_errno;

	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	*out = NULL;
	channel = calloc(1, sizeof(*channel));
	if (channel == NULL)
		return -1;
	channel->fd = -1;
	channel->lifetime_fd = -1;
	channel->source_lock_fd = -1;
	channel->destination_lock_fd = -1;
	channel->direction = direction;

	channel->lifetime_fd = tgbs_comm_open_contract(name, direction,
		TGBS_CONTRACT_QUEUING, &contract);
	if (channel->lifetime_fd < 0)
		goto error;
	channel->max_message_size = (size_t)contract.max_message_size;

	if (tgbs_comm_make_path(socket_path, sizeof(socket_path),
			name, TGBS_QUEUING_SOCKET) != 0)
		goto error;
	if (strlen(socket_path) >= sizeof(channel->socket_path)) {
		errno = ENAMETOOLONG;
		goto error;
	}
	memcpy(channel->socket_path, socket_path, strlen(socket_path) + 1);

	if (direction == TGBS_CHANNEL_SOURCE) {
		if (open_source(channel, name) != 0)
			goto error;
	} else {
		if (open_destination(channel, name) != 0)
			goto error;
	}

	*out = channel;
	return 0;

error:
	saved_errno = errno;
	tgbs_queuing_channel_close(channel);
	errno = saved_errno;
	return -1;
}

int tgbs_queuing_channel_send(tgbs_queuing_channel_t *channel, const void *message,
		size_t length, int64_t timeout_us)
{
	struct queuing_timeout timeout;
	struct timespec remaining;
	struct sockaddr_un address;
	socklen_t address_length;
	ssize_t sent;

	if (channel == NULL || channel->direction != TGBS_CHANNEL_SOURCE ||
	    message == NULL || length == 0) {
		errno = EINVAL;
		return -1;
	}
	if (length > channel->max_message_size) {
		errno = EMSGSIZE;
		return -1;
	}
	if (timeout_init(&timeout, timeout_us) != 0)
		return -1;
	address_length = unix_address(&address, channel->socket_path);
	for (;;) {
		/* Associate at each attempt, so source open is independent and the
		 * next send can find a recreated destination. All concurrent callers
		 * use the same immutable path; there is no userspace connected flag
		 * to race or become stale after fork. The peer association lets
		 * POLLOUT account for the destination queue as well as our buffer. */
		while (connect(channel->fd, (struct sockaddr *)&address,
				address_length) != 0) {
			if (errno != EINTR)
				goto error;
			if (timeout_remaining(&timeout, &remaining) != 0)
				return -1;
		}
		sent = send(channel->fd, message, length, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (sent >= 0)
			break;
		if (errno == EINTR) {
			if (timeout_remaining(&timeout, &remaining) != 0)
				return -1;
			continue;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			goto error;
		if (wait_ready(channel->fd, POLLOUT, &timeout) != 0)
			goto error;
	}
	if ((size_t)sent != length) {
		errno = EIO;
		return -1;
	}
	return 0;

error:
	/* Normalize unavailable-peer errors; preserve all other failures. */
	switch (errno) {
	case ENOENT:
	case ECONNREFUSED:
	case ECONNRESET:
	case EPIPE:
		errno = ENOTCONN;
		break;
	default:
		break;
	}
	return -1;
}

int tgbs_queuing_channel_receive(tgbs_queuing_channel_t *channel, void *message,
		size_t capacity, size_t *length, int64_t timeout_us)
{
	struct queuing_timeout timeout;
	struct timespec remaining;
	struct iovec iov;
	struct msghdr msg;
	ssize_t received;

	if (length != NULL)
		*length = 0;
	if (channel == NULL ||
	    channel->direction != TGBS_CHANNEL_DESTINATION ||
	    length == NULL || (message == NULL && capacity != 0)) {
		errno = EINVAL;
		return -1;
	}
	if (timeout_init(&timeout, timeout_us) != 0)
		return -1;
	memset(&msg, 0, sizeof(msg));
	iov.iov_base = message;
	iov.iov_len = capacity;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	for (;;) {
		received = recvmsg(channel->fd, &msg, MSG_TRUNC | MSG_DONTWAIT);
		if (received >= 0)
			break;
		if (errno == EINTR) {
			if (timeout_remaining(&timeout, &remaining) != 0)
				return -1;
			continue;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		if (wait_ready(channel->fd, POLLIN, &timeout) != 0)
			return -1;
	}
	*length = (size_t)received;
	if ((size_t)received > capacity) {
		errno = EMSGSIZE;
		return -1;
	}
	return 0;
}

size_t tgbs_queuing_channel_max_message_size(const tgbs_queuing_channel_t *channel)
{
	return channel != NULL ? channel->max_message_size : 0;
}

int tgbs_queuing_channel_fd(const tgbs_queuing_channel_t *channel)
{
	if (channel == NULL) {
		errno = EINVAL;
		return -1;
	}
	return channel->fd;
}

int tgbs_queuing_channel_get_status(tgbs_queuing_channel_t *channel,
		tgbs_queuing_channel_status_t *status)
{
	struct pollfd poll_fd;
	ssize_t next_size;

	if (channel == NULL || status == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(status, 0, sizeof(*status));
	status->direction = channel->direction;
	status->max_message_size = channel->max_message_size;
	if (channel->direction != TGBS_CHANNEL_DESTINATION)
		return 0;

	poll_fd.fd = channel->fd;
	poll_fd.events = POLLIN;
	poll_fd.revents = 0;
	if (poll(&poll_fd, 1, 0) < 0)
		return -1;
	if (!(poll_fd.revents & POLLIN))
		return 0;

	next_size = recv(channel->fd, NULL, 0,
		MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT);
	if (next_size < 0) {
		/* Another receiver may have consumed the message since poll(). */
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}
	status->message_pending = 1;
	status->next_message_size = (size_t)next_size;
	return 0;
}

void tgbs_queuing_channel_close(tgbs_queuing_channel_t *channel)
{
	if (channel == NULL)
		return;
	if (channel->direction == TGBS_CHANNEL_DESTINATION &&
	    channel->destination_lock_fd >= 0 && channel->socket_path[0] != '\0') {
		struct stat path;

		if (lstat(channel->socket_path, &path) == 0 &&
		    channel->socket_device == path.st_dev &&
		    channel->socket_inode == path.st_ino)
			unlink(channel->socket_path);
	}
	if (channel->fd >= 0)
		close(channel->fd);
	if (channel->source_lock_fd >= 0)
		close(channel->source_lock_fd);
	if (channel->destination_lock_fd >= 0)
		close(channel->destination_lock_fd);
	if (channel->lifetime_fd >= 0)
		close(channel->lifetime_fd);
	free(channel);
}
