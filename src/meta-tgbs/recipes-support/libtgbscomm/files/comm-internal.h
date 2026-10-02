/* SPDX-License-Identifier: MIT */
#ifndef TGBS_COMM_INTERNAL_H
#define TGBS_COMM_INTERNAL_H

#include "channel-contract.h"
#include "types.h"

#ifndef TGBS_CHANNEL_ROOT
#define TGBS_CHANNEL_ROOT "/run/tgbs/channels"
#endif

int tgbs_comm_make_path(char *out, size_t capacity, const char *name,
	const char *suffix);
/* Validate the direction/type and hold lifetime.lock while reading a contract.
 * On success the caller owns the returned fd; on failure no lock is retained. */
int tgbs_comm_open_contract(const char *name, tgbs_channel_direction_t direction,
	enum tgbs_contract_type type, struct tgbs_channel_contract *contract);

#endif
