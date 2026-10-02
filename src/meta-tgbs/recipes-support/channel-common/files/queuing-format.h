/* SPDX-License-Identifier: MIT */
#ifndef TGBS_QUEUING_FORMAT_H
#define TGBS_QUEUING_FORMAT_H

/* Queuing messages are AF_UNIX datagrams. Only the socket pathname is shared
 * with the channel manager; there is no on-disk message format. */
#define TGBS_QUEUING_SOCKET_NAME "channel.sock"
#define TGBS_QUEUING_SOCKET "/endpoint/" TGBS_QUEUING_SOCKET_NAME

#endif
