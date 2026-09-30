/*
 * SPDX-License-Identifier: MIT
 *
 * Bidirectional libtgbscomm ping-pong demonstration node.
 */

#include <tgbs/channel.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MESSAGE_SIZE 128

static int send_message(tgbs_channel_t *channel, const char *node,
		const char *channel_name, const char *message)
{
	struct timespec retry = {
		.tv_sec = 0,
		.tv_nsec = 100000000,
	};
	int waiting = 0;

	for (;;) {
		if (tgbs_channel_send(channel, message, strlen(message)) == 0) {
			printf("%s: sent on %s: %s\n", node, channel_name, message);
			return 0;
		}
		if (errno != ENOENT && errno != ECONNREFUSED)
			return -1;
		if (!waiting) {
			printf("%s: waiting for the receiver on %s\n",
				node, channel_name);
			waiting = 1;
		}
		nanosleep(&retry, NULL);
	}
}

static int receive_message(tgbs_channel_t *channel, const char *node,
		const char *channel_name, char *message, size_t capacity)
{
	size_t length;

	if (tgbs_channel_receive(channel, message, capacity - 1, &length) != 0)
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
	tgbs_channel_t *receiver = NULL;
	tgbs_channel_t *sender = NULL;
	char message[MESSAGE_SIZE + 1];
	unsigned int sequence = 1;
	int initiator;

	if (argc != 5) {
		fprintf(stderr,
			"Usage: comm-node initiator|responder NODE RECEIVE_CHANNEL SEND_CHANNEL\n");
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

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (tgbs_channel_open(receive_channel, TGBS_CHANNEL_DESTINATION,
			&receiver) != 0) {
		fprintf(stderr, "%s: cannot open receiver %s: %s\n",
			node, receive_channel, strerror(errno));
		return 1;
	}
	if (tgbs_channel_open(send_channel, TGBS_CHANNEL_SOURCE, &sender) != 0) {
		fprintf(stderr, "%s: cannot open sender %s: %s\n",
			node, send_channel, strerror(errno));
		tgbs_channel_close(receiver);
		return 1;
	}

	printf("%s: ready, receive=%s, send=%s\n",
		node, receive_channel, send_channel);
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

	fprintf(stderr, "%s: communication failed: %s\n", node, strerror(errno));
	tgbs_channel_close(sender);
	tgbs_channel_close(receiver);
	return 1;
}
