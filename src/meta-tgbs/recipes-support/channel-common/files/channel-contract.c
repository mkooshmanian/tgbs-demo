/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "channel-contract.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *tgbs_contract_type_name(enum tgbs_contract_type type)
{
	switch (type) {
	case TGBS_CONTRACT_QUEUING: return "queuing";
	case TGBS_CONTRACT_SAMPLING: return "sampling";
	default: return "unknown";
	}
}

int tgbs_contract_parse_type(const char *text, enum tgbs_contract_type *type)
{
	if (strcmp(text, "queuing") == 0)
		*type = TGBS_CONTRACT_QUEUING;
	else if (strcmp(text, "sampling") == 0)
		*type = TGBS_CONTRACT_SAMPLING;
	else {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int tgbs_contract_parse_positive(const char *text, unsigned long long *out)
{
	const unsigned char *p = (const unsigned char *)text;
	char *end;
	unsigned long long value;

	if (*p == '\0')
		goto invalid;
	for (; *p != '\0'; p++)
		if (!isdigit(*p))
			goto invalid;
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || *end != '\0' || value == 0)
		goto invalid;
	*out = value;
	return 0;
invalid:
	errno = EINVAL;
	return -1;
}

int tgbs_contract_valid_name(const char *name, size_t capacity)
{
	const unsigned char *p = (const unsigned char *)name;

	if (name == NULL || name[0] == '\0' || strlen(name) >= capacity ||
	    strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return 0;
	for (; *p != '\0'; p++)
		if (!isalnum(*p) && *p != '_' && *p != '.' && *p != '-')
			return 0;
	return 1;
}

int tgbs_contract_has_destination(const struct tgbs_channel_contract *contract,
	const char *domain)
{
	size_t i;

	for (i = 0; i < contract->destination_count; i++)
		if (strcmp(contract->destinations[i], domain) == 0)
			return 1;
	return 0;
}

int tgbs_contract_add_destination(struct tgbs_channel_contract *contract,
	const char *domain)
{
	if (!tgbs_contract_valid_name(domain, TGBS_CONTRACT_DOMAIN_SIZE) ||
	    strcmp(domain, "channels") == 0 ||
	    contract->destination_count >= TGBS_CONTRACT_MAX_DESTINATIONS ||
	    tgbs_contract_has_destination(contract, domain)) {
		errno = EINVAL;
		return -1;
	}
	strcpy(contract->destinations[contract->destination_count++], domain);
	return 0;
}

int tgbs_contract_validate(const struct tgbs_channel_contract *contract)
{
	size_t i, j;

	if (!tgbs_contract_valid_name(contract->name, TGBS_CONTRACT_NAME_SIZE) ||
	    contract->generation[0] == '\0' ||
	    !tgbs_contract_valid_name(contract->source, TGBS_CONTRACT_DOMAIN_SIZE) ||
	    strcmp(contract->source, "channels") == 0 ||
	    contract->destination_count == 0 ||
	    contract->destination_count > TGBS_CONTRACT_MAX_DESTINATIONS ||
	    contract->max_message_size == 0 ||
	    contract->max_message_size > (unsigned long long)(INT_MAX - 32))
		goto invalid;
	for (i = 0; i < contract->destination_count; i++) {
		if (!tgbs_contract_valid_name(contract->destinations[i],
				TGBS_CONTRACT_DOMAIN_SIZE) ||
		    strcmp(contract->destinations[i], "channels") == 0)
			goto invalid;
		for (j = 0; j < i; j++)
			if (strcmp(contract->destinations[i], contract->destinations[j]) == 0)
				goto invalid;
	}
	if (contract->type == TGBS_CONTRACT_QUEUING) {
		if (contract->destination_count != 1 || contract->refresh_period_us != 0)
			goto invalid;
	} else if (contract->type == TGBS_CONTRACT_SAMPLING) {
		if (contract->refresh_period_us == 0 ||
		    contract->refresh_period_us > UINT64_MAX / 1000)
			goto invalid;
	} else {
		goto invalid;
	}
	return 0;
invalid:
	errno = EINVAL;
	return -1;
}

static int copy_value(char *out, size_t capacity, const char *value)
{
	size_t length = strlen(value);

	if (length == 0 || length >= capacity)
		return -1;
	memcpy(out, value, length + 1);
	return 0;
}

int tgbs_contract_read(const char *root, const char *name,
	struct tgbs_channel_contract *contract)
{
	char path[PATH_MAX];
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length;
	unsigned int seen = 0;
	unsigned long long format = 0;
	FILE *file;
	int n;

	if (!tgbs_contract_valid_name(name, TGBS_CONTRACT_NAME_SIZE)) {
		errno = EINVAL;
		return -1;
	}
	n = snprintf(path, sizeof(path), "%s/%s/contract", root, name);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	file = fopen(path, "r");
	if (file == NULL)
		return -1;
	memset(contract, 0, sizeof(*contract));
	contract->type = TGBS_CONTRACT_QUEUING;
	while ((length = getline(&line, &capacity, file)) >= 0) {
		char *equals, *key, *value;

		/* Reject embedded NULs rather than silently ignoring trailing data. */
		if (memchr(line, '\0', (size_t)length) != NULL)
			goto invalid;
		while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
			line[--length] = '\0';
		equals = strchr(line, '=');
		if (equals == NULL || equals == line || equals[1] == '\0')
			goto invalid;
		*equals = '\0';
		key = line;
		value = equals + 1;
		if (strcmp(key, "format") == 0 && !(seen & 1U)) {
			if (tgbs_contract_parse_positive(value, &format) != 0)
				goto invalid;
			seen |= 1U;
		} else if (strcmp(key, "name") == 0 && !(seen & 2U)) {
			if (copy_value(contract->name, sizeof(contract->name), value) != 0)
				goto invalid;
			seen |= 2U;
		} else if (strcmp(key, "generation") == 0 && !(seen & 4U)) {
			if (copy_value(contract->generation, sizeof(contract->generation), value) != 0)
				goto invalid;
			seen |= 4U;
		} else if (strcmp(key, "source") == 0 && !(seen & 8U)) {
			if (copy_value(contract->source, sizeof(contract->source), value) != 0)
				goto invalid;
			seen |= 8U;
		} else if (strcmp(key, "destination") == 0) {
			if (tgbs_contract_add_destination(contract, value) != 0)
				goto invalid;
			seen |= 16U;
		} else if (strcmp(key, "max_message_size") == 0 && !(seen & 32U)) {
			if (tgbs_contract_parse_positive(value, &contract->max_message_size) != 0)
				goto invalid;
			seen |= 32U;
		} else if (strcmp(key, "type") == 0 && !(seen & 64U)) {
			if (tgbs_contract_parse_type(value, &contract->type) != 0)
				goto invalid;
			seen |= 64U;
		} else if (strcmp(key, "refresh_period_us") == 0 && !(seen & 128U)) {
			if (tgbs_contract_parse_positive(value, &contract->refresh_period_us) != 0)
				goto invalid;
			seen |= 128U;
		} else {
			goto invalid;
		}
	}
	if (ferror(file))
		goto invalid;
	if (format == 1) {
		if (seen != 63U)
			goto invalid;
	} else if (format == TGBS_CONTRACT_FORMAT) {
		if (seen != (contract->type == TGBS_CONTRACT_SAMPLING ? 255U : 127U))
			goto invalid;
	} else {
		goto invalid;
	}
	if (strcmp(contract->name, name) != 0 || tgbs_contract_validate(contract) != 0)
		goto invalid;
	free(line);
	fclose(file);
	return 0;
invalid:
	free(line);
	fclose(file);
	errno = EINVAL;
	return -1;
}
