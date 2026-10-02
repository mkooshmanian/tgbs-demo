/* SPDX-License-Identifier: MIT */
#ifndef TGBS_CHANNEL_CONTRACT_H
#define TGBS_CHANNEL_CONTRACT_H

#include <stddef.h>

#define TGBS_CONTRACT_FORMAT 2
#define TGBS_CONTRACT_NAME_SIZE 64
#define TGBS_CONTRACT_DOMAIN_SIZE 256
#define TGBS_CONTRACT_MAX_DESTINATIONS 64

enum tgbs_contract_type {
	TGBS_CONTRACT_QUEUING = 1,
	TGBS_CONTRACT_SAMPLING
};

struct tgbs_channel_contract {
	enum tgbs_contract_type type;
	char name[TGBS_CONTRACT_NAME_SIZE];
	char generation[64];
	char source[TGBS_CONTRACT_DOMAIN_SIZE];
	char destinations[TGBS_CONTRACT_MAX_DESTINATIONS][TGBS_CONTRACT_DOMAIN_SIZE];
	size_t destination_count;
	unsigned long long max_message_size;
	unsigned long long refresh_period_us;
};

const char *tgbs_contract_type_name(enum tgbs_contract_type type);
int tgbs_contract_parse_type(const char *text, enum tgbs_contract_type *type);
int tgbs_contract_parse_positive(const char *text, unsigned long long *out);
int tgbs_contract_valid_name(const char *name, size_t capacity);
int tgbs_contract_add_destination(struct tgbs_channel_contract *contract,
	const char *domain);
int tgbs_contract_has_destination(const struct tgbs_channel_contract *contract,
	const char *domain);
int tgbs_contract_validate(const struct tgbs_channel_contract *contract);
/* Format 1 is interpreted as queuing; new contracts use format 2. */
int tgbs_contract_read(const char *root, const char *name,
	struct tgbs_channel_contract *contract);

#endif
