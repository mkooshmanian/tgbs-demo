/*
 * SPDX-License-Identifier: MIT
 *
 * AF_UNIX SOCK_DGRAM backend for immutable TGBS communication channels.
 */

#define _GNU_SOURCE
#include "channel.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef TGBS_CHANNEL_ROOT
#define TGBS_CHANNEL_ROOT "/run/tgbs/channels"
#endif

#define TGBS_CHANNEL_NAME_SIZE 64
#define CONTRACT_FILE "/contract"
#define LIFETIME_LOCK "/lifetime.lock"
#define SOURCE_LOCK "/endpoint/source.lock"
#define RECEIVER_LOCK "/endpoint/receiver.lock"
#define CHANNEL_SOCKET "/endpoint/channel.sock"
#define CONTRACT_FORMAT 1

struct channel_contract {
	char name[TGBS_CHANNEL_NAME_SIZE];
	char source[256];
	char destination[256];
	unsigned long long max_message_size;
};

struct tgbs_channel {
	int fd;
	int lifetime_fd;
	int source_lock_fd;
	int receiver_lock_fd;
	tgbs_channel_direction_t direction;
	size_t max_message_size;
	char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	dev_t socket_device;
	ino_t socket_inode;
};

static int valid_name(const char *name)
{
	size_t i;
	size_t length;

	if (name == NULL || name[0] == '\0' ||
	    strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return 0;
	length = strlen(name);
	if (length >= TGBS_CHANNEL_NAME_SIZE)
		return 0;
	for (i = 0; i < length; i++) {
		unsigned char c = (unsigned char)name[i];

		if (!isalnum(c) && c != '_' && c != '.' && c != '-')
			return 0;
	}
	return 1;
}

static int make_path(char *out, size_t outsz, const char *name,
		const char *suffix)
{
	int length = snprintf(out, outsz, "%s/%s%s",
		TGBS_CHANNEL_ROOT, name, suffix);

	if (length < 0 || (size_t)length >= outsz) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int parse_positive(const char *text, unsigned long long *out)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value == 0) {
		errno = EINVAL;
		return -1;
	}
	*out = value;
	return 0;
}

static int copy_value(char *out, size_t outsz, const char *value)
{
	size_t length = strlen(value);

	if (length == 0 || length >= outsz) {
		errno = EINVAL;
		return -1;
	}
	memcpy(out, value, length + 1);
	return 0;
}

static int read_contract(const char *name, struct channel_contract *contract)
{
	char path[PATH_MAX];
	char generation[64] = "";
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length;
	unsigned long long format = 0;
	unsigned int seen = 0;
	FILE *file;

	if (make_path(path, sizeof(path), name, CONTRACT_FILE) != 0)
		return -1;
	file = fopen(path, "r");
	if (file == NULL)
		return -1;
	memset(contract, 0, sizeof(*contract));

	while ((length = getline(&line, &capacity, file)) >= 0) {
		char *equals;
		char *key;
		char *value;

		while (length > 0 &&
		       (line[length - 1] == '\n' || line[length - 1] == '\r'))
			line[--length] = '\0';
		equals = strchr(line, '=');
		if (equals == NULL || equals == line || equals[1] == '\0')
			goto invalid;
		*equals = '\0';
		key = line;
		value = equals + 1;

		if (strcmp(key, "format") == 0 && !(seen & 1U)) {
			if (parse_positive(value, &format) != 0)
				goto invalid;
			seen |= 1U;
		} else if (strcmp(key, "name") == 0 && !(seen & 2U)) {
			if (copy_value(contract->name, sizeof(contract->name), value) != 0)
				goto invalid;
			seen |= 2U;
		} else if (strcmp(key, "generation") == 0 && !(seen & 4U)) {
			if (copy_value(generation, sizeof(generation), value) != 0)
				goto invalid;
			seen |= 4U;
		} else if (strcmp(key, "source") == 0 && !(seen & 8U)) {
			if (copy_value(contract->source,
					sizeof(contract->source), value) != 0)
				goto invalid;
			seen |= 8U;
		} else if (strcmp(key, "destination") == 0 && !(seen & 16U)) {
			if (copy_value(contract->destination,
					sizeof(contract->destination), value) != 0)
				goto invalid;
			seen |= 16U;
		} else if (strcmp(key, "max_message_size") == 0 && !(seen & 32U)) {
			if (parse_positive(value, &contract->max_message_size) != 0)
				goto invalid;
			seen |= 32U;
		} else {
			goto invalid;
		}
	}
	if (ferror(file))
		goto invalid;
	free(line);
	fclose(file);

	if (seen != 63U || format != CONTRACT_FORMAT ||
	    strcmp(contract->name, name) != 0 ||
	    !valid_name(contract->name) ||
	    !valid_name(contract->source) ||
	    !valid_name(contract->destination) ||
	    contract->max_message_size > (unsigned long long)(INT_MAX - 32)) {
		errno = EINVAL;
		return -1;
	}
	return 0;

invalid:
	free(line);
	fclose(file);
	errno = EINVAL;
	return -1;
}

static int open_lifetime_lock(const char *name)
{
	char path[PATH_MAX];
	int fd;

	if (make_path(path, sizeof(path), name, LIFETIME_LOCK) != 0)
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (flock(fd, LOCK_SH) != 0) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}
	return fd;
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

	if (make_path(path, sizeof(path), name, suffix) != 0)
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

static int open_source(struct tgbs_channel *channel, const char *name)
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

static int open_destination(struct tgbs_channel *channel, const char *name)
{
	struct sockaddr_un address;
	socklen_t address_length;
	struct stat socket_stat;

	channel->receiver_lock_fd = open_endpoint_lock(name, RECEIVER_LOCK);
	if (channel->receiver_lock_fd < 0)
		return -1;

	channel->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (channel->fd < 0)
		return -1;

	/* Holding receiver.lock proves that a socket left at this pathname is
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

int tgbs_channel_open(const char *name, tgbs_channel_direction_t direction,
		tgbs_channel_t **out)
{
	struct channel_contract contract;
	struct tgbs_channel *channel;
	char socket_path[PATH_MAX];
	int saved_errno;

	if (out == NULL || !valid_name(name) ||
	    (direction != TGBS_CHANNEL_SOURCE &&
	     direction != TGBS_CHANNEL_DESTINATION)) {
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
	channel->receiver_lock_fd = -1;
	channel->direction = direction;

	channel->lifetime_fd = open_lifetime_lock(name);
	if (channel->lifetime_fd < 0)
		goto error;
	if (read_contract(name, &contract) != 0)
		goto error;
	channel->max_message_size = (size_t)contract.max_message_size;

	if (make_path(socket_path, sizeof(socket_path),
			name, CHANNEL_SOCKET) != 0)
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
	tgbs_channel_close(channel);
	errno = saved_errno;
	return -1;
}

int tgbs_channel_send(tgbs_channel_t *channel, const void *message,
		size_t length)
{
	ssize_t sent;
	struct sockaddr_un address;
	socklen_t address_length;

	if (channel == NULL || channel->direction != TGBS_CHANNEL_SOURCE ||
	    message == NULL || length == 0) {
		errno = EINVAL;
		return -1;
	}
	if (length > channel->max_message_size) {
		errno = EMSGSIZE;
		return -1;
	}
	address_length = unix_address(&address, channel->socket_path);
	do {
		sent = sendto(channel->fd, message, length, MSG_NOSIGNAL,
			(struct sockaddr *)&address, address_length);
	} while (sent < 0 && errno == EINTR);
	if (sent < 0)
		return -1;
	if ((size_t)sent != length) {
		errno = EIO;
		return -1;
	}
	return 0;
}

int tgbs_channel_receive(tgbs_channel_t *channel, void *message,
		size_t capacity, size_t *length)
{
	struct iovec iov;
	struct msghdr msg;
	ssize_t received;

	if (channel == NULL ||
	    channel->direction != TGBS_CHANNEL_DESTINATION ||
	    length == NULL || (message == NULL && capacity != 0)) {
		errno = EINVAL;
		return -1;
	}
	memset(&msg, 0, sizeof(msg));
	iov.iov_base = message;
	iov.iov_len = capacity;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	do {
		received = recvmsg(channel->fd, &msg, MSG_TRUNC);
	} while (received < 0 && errno == EINTR);
	if (received < 0)
		return -1;
	*length = (size_t)received;
	if ((size_t)received > capacity) {
		errno = EMSGSIZE;
		return -1;
	}
	return 0;
}

size_t tgbs_channel_max_message_size(const tgbs_channel_t *channel)
{
	return channel != NULL ? channel->max_message_size : 0;
}

int tgbs_channel_fd(const tgbs_channel_t *channel)
{
	if (channel == NULL) {
		errno = EINVAL;
		return -1;
	}
	return channel->fd;
}

int tgbs_channel_get_status(tgbs_channel_t *channel,
		tgbs_channel_status_t *status)
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
	if (next_size < 0)
		return -1;
	status->message_pending = 1;
	status->next_message_size = (size_t)next_size;
	return 0;
}

void tgbs_channel_close(tgbs_channel_t *channel)
{
	if (channel == NULL)
		return;
	if (channel->direction == TGBS_CHANNEL_DESTINATION &&
	    channel->receiver_lock_fd >= 0 && channel->socket_path[0] != '\0') {
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
	if (channel->receiver_lock_fd >= 0)
		close(channel->receiver_lock_fd);
	if (channel->lifetime_fd >= 0)
		close(channel->lifetime_fd);
	free(channel);
}
