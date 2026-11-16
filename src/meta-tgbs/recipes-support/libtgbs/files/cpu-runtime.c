/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "cpu-runtime.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void free_runtime(struct runtime_config *config)
{
	free(config->overrides);
	memset(config, 0, sizeof(*config));
}

int parse_runtime_value(const char *text, unsigned long long *out)
{
	char *end;
	unsigned long long value;

	if (!isdigit((unsigned char)text[0]))
		return -1;
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || *end != '\0')
		return -1;
	*out = value;
	return 0;
}

int read_runtime(const char *path, struct runtime_config *config)
{
	FILE *f;
	char *line = NULL;
	size_t capacity = 0;
	int have_default = 0;
	int saved_errno;

	memset(config, 0, sizeof(*config));
	f = fopen(path, "r");
	if (f == NULL)
		return -1;

	while (getline(&line, &capacity, f) >= 0) {
		char *save;
		char *key = strtok_r(line, " \t\r\n", &save);
		char *value = strtok_r(NULL, " \t\r\n", &save);
		unsigned long long runtime, cpu;
		struct cpu_runtime *larger;

		errno = EINVAL;
		if (key == NULL || strtok_r(NULL, " \t\r\n", &save) != NULL)
			goto fail;
		if (value == NULL || strcmp(key, "default") == 0) {
			if (have_default || config->count != 0 ||
			    parse_runtime_value(value != NULL ? value : key, &runtime) != 0)
				goto fail;
			config->default_runtime = runtime;
			have_default = 1;
			continue;
		}
		if (!have_default || parse_runtime_value(key, &cpu) != 0 || cpu > UINT_MAX ||
		    parse_runtime_value(value, &runtime) != 0)
			goto fail;
		/* The kernel emits each override once, in ascending CPU order. */
		if (config->count != 0 && cpu <= config->overrides[config->count - 1].cpu)
			goto fail;
		larger = realloc(config->overrides,
			(config->count + 1) * sizeof(*larger));
		if (larger == NULL)
			goto fail;
		config->overrides = larger;
		config->overrides[config->count++] = (struct cpu_runtime) {
			.cpu = (unsigned int)cpu, .runtime = runtime
		};
	}
	if (!feof(f))
		goto fail;
	if (!have_default) {
		errno = EINVAL;
		goto fail;
	}
	free(line);
	fclose(f);
	return 0;

fail:
	saved_errno = errno != 0 ? errno : EINVAL;
	free(line);
	fclose(f);
	free_runtime(config);
	errno = saved_errno;
	return -1;
}

unsigned long long max_runtime(const struct runtime_config *config)
{
	unsigned long long maximum = config->default_runtime;

	for (size_t i = 0; i < config->count; i++)
		if (config->overrides[i].runtime > maximum)
			maximum = config->overrides[i].runtime;
	return maximum;
}

unsigned long long runtime_for_cpu(const struct runtime_config *config,
		unsigned int cpu)
{
	for (size_t i = 0; i < config->count; i++) {
		if (config->overrides[i].cpu == cpu)
			return config->overrides[i].runtime;
		if (config->overrides[i].cpu > cpu)
			break;
	}
	return config->default_runtime;
}

void free_cpu_list(struct cpu_list *list)
{
	free(list->cpus);
	memset(list, 0, sizeof(*list));
}

int parse_cpu_list(const char *text, struct cpu_list *list)
{
	const char *p = text;

	memset(list, 0, sizeof(*list));
	while (*p != '\0' && *p != '\n') {
		char *end;
		unsigned long first, last;
		size_t count;
		unsigned int *larger;

		errno = EINVAL;
		if (!isdigit((unsigned char)*p))
			goto fail;
		errno = 0;
		first = strtoul(p, &end, 10);
		if (errno != 0 || first > UINT_MAX)
			goto fail;
		p = end;
		last = first;
		if (*p == '-') {
			p++;
			if (!isdigit((unsigned char)*p))
				goto fail;
			last = strtoul(p, &end, 10);
			if (errno != 0 || last > UINT_MAX || last < first)
				goto fail;
			p = end;
		}
		if (list->count != 0 && first <= list->cpus[list->count - 1])
			goto fail;
		if ((unsigned long long)last - first + 1 >
		    SIZE_MAX / sizeof(*larger) - list->count) {
			errno = EOVERFLOW;
			goto fail;
		}
		count = (size_t)((unsigned long long)last - first + 1);
		larger = realloc(list->cpus, (list->count + count) * sizeof(*larger));
		if (larger == NULL)
			goto fail;
		list->cpus = larger;
		for (size_t i = 0; i < count; i++)
			list->cpus[list->count++] = (unsigned int)(first + i);
		if (*p != ',')
			break;
		p++;
		if (!isdigit((unsigned char)*p))
			goto fail;
	}
	while (isspace((unsigned char)*p))
		p++;
	if (*p != '\0')
		goto fail;
	return 0;

fail:
	free_cpu_list(list);
	if (errno == 0)
		errno = EINVAL;
	return -1;
}

int read_possible_cpus(struct cpu_list *list)
{
	FILE *f;
	char *text = NULL;
	size_t size = 0;
	int result = -1;
	int saved_errno;

	memset(list, 0, sizeof(*list));
	f = fopen(CPU_POSSIBLE_PATH, "r");
	if (f == NULL)
		return -1;
	if (getline(&text, &size, f) >= 0) {
		result = parse_cpu_list(text, list);
		if (result == 0 && list->count == 0) {
			errno = EINVAL;
			result = -1;
		}
	} else if (feof(f)) {
		errno = EINVAL;
	}
	saved_errno = errno;
	free(text);
	fclose(f);
	errno = saved_errno;
	return result;
}

long double total_runtime(const struct runtime_config *config,
		const struct cpu_list *list)
{
	long double total = 0.0L;

	for (size_t i = 0; i < list->count; i++)
		total += runtime_for_cpu(config, list->cpus[i]);
	return total;
}

