/*
 * SPDX-License-Identifier: MIT
 *
 * Queuing ping-pong and sampling publication/subscription demonstration node.
 */

#include <tgbs/queuing.h>
#include <tgbs/sampling.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MESSAGE_SIZE 128

struct sampling_reader {
	tgbs_sampling_channel_t *channel;
	const char *node;
	const char *channel_name;
	atomic_int stop;
};

static void *read_samples(void *arg)
{
	struct sampling_reader *reader = arg;
	const struct timespec interval = { .tv_sec = 0, .tv_nsec = 500000000 };
	char message[MESSAGE_SIZE + 1];

	while (!atomic_load(&reader->stop)) {
		size_t length;
		tgbs_sampling_validity_t validity;

		if (tgbs_sampling_channel_read(reader->channel, message,
				MESSAGE_SIZE, &length, &validity) == 0) {
			message[length] = '\0';
			printf("%s: sampling on %s [%s]: %s\n", reader->node,
				reader->channel_name,
				validity == TGBS_SAMPLING_VALID ? "VALID" : "INVALID",
				message);
		} else if (errno == ENODATA) {
			printf("%s: sampling on %s: no publication yet\n",
				reader->node, reader->channel_name);
		} else {
			fprintf(stderr, "%s: sampling read failed on %s: %s\n",
				reader->node, reader->channel_name, strerror(errno));
			exit(EXIT_FAILURE);
		}
		nanosleep(&interval, NULL);
	}
	return NULL;
}

static int publish_samples(const char *node, const char *channel_name)
{
	tgbs_sampling_channel_t *channel;
	char message[MESSAGE_SIZE + 1];
	unsigned int sequence = 1;
	int saved_errno;

	if (tgbs_sampling_channel_open(channel_name, TGBS_CHANNEL_SOURCE,
			&channel) != 0) {
		fprintf(stderr, "%s: cannot open sampling source %s: %s\n",
			node, channel_name, strerror(errno));
		return 1;
	}
	printf("%s: ready, sampling source=%s\n", node, channel_name);
	for (;;) {
		snprintf(message, sizeof(message), "sample %u from %s", sequence++, node);
		if (tgbs_sampling_channel_write(channel, message, strlen(message)) != 0)
			break;
		printf("%s: published on %s: %s\n", node, channel_name, message);
		sleep(1);
	}
	saved_errno = errno;
	tgbs_sampling_channel_close(channel);
	fprintf(stderr, "%s: sampling publication failed: %s\n",
		node, strerror(saved_errno));
	return 1;
}

static int send_message(tgbs_queuing_channel_t *channel, const char *node,
		const char *channel_name, const char *message)
{
	struct timespec retry = {
		.tv_sec = 0,
		.tv_nsec = 100000000,
	};
	int waiting = 0;

	for (;;) {
		if (tgbs_queuing_channel_send(channel, message, strlen(message),
				TGBS_TIMEOUT_INFINITE) == 0) {
			printf("%s: sent on %s: %s\n", node, channel_name, message);
			return 0;
		}
		if (errno != ENOTCONN)
			return -1;
		if (!waiting) {
			printf("%s: waiting for the receiver on %s\n",
				node, channel_name);
			waiting = 1;
		}
		nanosleep(&retry, NULL);
	}
}

static int receive_message(tgbs_queuing_channel_t *channel, const char *node,
		const char *channel_name, char *message, size_t capacity)
{
	size_t length;

	if (tgbs_queuing_channel_receive(channel, message, capacity - 1, &length,
			TGBS_TIMEOUT_INFINITE) != 0)
		return -1;
	message[length] = '\0';
	printf("%s: received on %s: %s\n", node, channel_name, message);
	return 0;
}

int main(int argc, char **argv)
{
	const char *role;
	const char *node;
	const char *receive_channel;
	const char *send_channel;
	tgbs_queuing_channel_t *receiver = NULL;
	tgbs_queuing_channel_t *sender = NULL;
	struct sampling_reader sampling = { .stop = ATOMIC_VAR_INIT(0) };
	pthread_t sampling_thread;
	char message[MESSAGE_SIZE + 1];
	unsigned int sequence = 1;
	int initiator;
	int saved_errno;
	int error;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 4 && strcmp(argv[1], "publisher") == 0)
		return publish_samples(argv[2], argv[3]);
	if (argc != 6) {
		fprintf(stderr,
			"Usage: comm-node initiator|responder NODE RECEIVE_CHANNEL SEND_CHANNEL SAMPLING_CHANNEL\n"
			"       comm-node publisher NODE SAMPLING_CHANNEL\n");
		return 2;
	}
	role = argv[1];
	node = argv[2];
	receive_channel = argv[3];
	send_channel = argv[4];
	if (strcmp(role, "initiator") == 0)
		initiator = 1;
	else if (strcmp(role, "responder") == 0)
		initiator = 0;
	else {
		fprintf(stderr, "comm-node: invalid role: %s\n", role);
		return 2;
	}

	if (tgbs_queuing_channel_open(receive_channel, TGBS_CHANNEL_DESTINATION,
			&receiver) != 0) {
		fprintf(stderr, "%s: cannot open receiver %s: %s\n",
			node, receive_channel, strerror(errno));
		return 1;
	}
	if (tgbs_queuing_channel_open(send_channel, TGBS_CHANNEL_SOURCE, &sender) != 0) {
		fprintf(stderr, "%s: cannot open sender %s: %s\n",
			node, send_channel, strerror(errno));
		tgbs_queuing_channel_close(receiver);
		return 1;
	}
	sampling.node = node;
	sampling.channel_name = argv[5];
	if (tgbs_sampling_channel_open(sampling.channel_name,
			TGBS_CHANNEL_DESTINATION, &sampling.channel) != 0) {
		fprintf(stderr, "%s: cannot open sampling destination %s: %s\n",
			node, sampling.channel_name, strerror(errno));
		tgbs_queuing_channel_close(sender);
		tgbs_queuing_channel_close(receiver);
		return 1;
	}
	error = pthread_create(&sampling_thread, NULL, read_samples, &sampling);
	if (error != 0) {
		fprintf(stderr, "%s: cannot start sampling reader: %s\n",
			node, strerror(error));
		tgbs_sampling_channel_close(sampling.channel);
		tgbs_queuing_channel_close(sender);
		tgbs_queuing_channel_close(receiver);
		return 1;
	}

	printf("%s: ready, receive=%s, send=%s, sampling=%s\n",
		node, receive_channel, send_channel, sampling.channel_name);
	for (;;) {
		if (initiator) {
			snprintf(message, sizeof(message), "ping %u from %s",
				sequence, node);
			if (send_message(sender, node, send_channel, message) != 0)
				break;
			if (receive_message(receiver, node, receive_channel,
					message, sizeof(message)) != 0)
				break;
			sleep(1);
		} else {
			if (receive_message(receiver, node, receive_channel,
					message, sizeof(message)) != 0)
				break;
			snprintf(message, sizeof(message), "pong %u from %s",
				sequence, node);
			if (send_message(sender, node, send_channel, message) != 0)
				break;
		}
		sequence++;
	}

	saved_errno = errno;
	atomic_store(&sampling.stop, 1);
	pthread_join(sampling_thread, NULL);
	fprintf(stderr, "%s: communication failed: %s\n", node, strerror(saved_errno));
	tgbs_sampling_channel_close(sampling.channel);
	tgbs_queuing_channel_close(sender);
	tgbs_queuing_channel_close(receiver);
	return 1;
}
