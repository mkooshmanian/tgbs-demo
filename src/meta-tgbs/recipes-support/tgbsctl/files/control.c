/*
 * SPDX-License-Identifier: MIT
 *
 * tgbsctl mutating commands for active TGBS domains.
 *
 * These operations act directly on cgroupfs. The tgbsctl run process remains
 * the lifecycle supervisor and is responsible for removing its cgroup and
 * runtime marker after the main process exits.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#define FREEZE_RETRIES 500
#define FREEZE_RETRY_US 10000

static int require_domain(const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	if (!is_valid_name(name)) {
		fprintf(stderr,
			"tgbsctl: ERROR - NAME must match [a-zA-Z0-9_.-]+ (max 255, no '/'); got '%s'\n",
			name == NULL ? "(null)" : name);
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "tgbsctl: ERROR - TGBS domain %s does not exist\n", name);
		return -1;
	}

	/* Avoid mutating an unrelated cgroup that happens to have the same name. */
	snprintf(path, sizeof(path), "%s/%s", RUN_ROOT, name);
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "tgbsctl: ERROR - cgroup %s is not managed by tgbsctl\n", name);
		return -1;
	}

	/* Do not race run while it is still configuring the cgroup and publishing
	 * its main-process marker. */
	snprintf(path, sizeof(path), "%s/%s/main.pid", RUN_ROOT, name);
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		fprintf(stderr, "tgbsctl: ERROR - TGBS domain %s is not ready\n", name);
		return -1;
	}
	return 0;
}

static int read_event(const char *name, const char *key, int *out)
{
	char path[PATH_MAX];
	char line[128];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s/cgroup.events", CG_ROOT, name);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;

	while (fgets(line, sizeof(line), f) != NULL) {
		char event[64];
		int value;

		if (sscanf(line, "%63s %d", event, &value) == 2 &&
		    strcmp(event, key) == 0) {
			fclose(f);
			*out = value;
			return 0;
		}
	}
	fclose(f);
	errno = ENOENT;
	return -1;
}

int cmd_kill(const char *name)
{
	if (require_domain(name) != 0)
		return 1;
	if (cgroup_kill(name) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to kill processes in %s: %s\n",
			name, strerror(errno));
		return 1;
	}
	printf("tgbsctl: killed all processes in %s\n", name);
	return 0;
}

int cmd_freeze(const char *name, int freeze)
{
	char path[PATH_MAX];
	const char *operation = freeze ? "pause" : "resume";
	int frozen;

	if (require_domain(name) != 0)
		return 1;

	snprintf(path, sizeof(path), "%s/%s/cgroup.freeze", CG_ROOT, name);
	if (write_u64(path, freeze ? 1 : 0) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to %s %s: %s\n",
			operation, name, strerror(errno));
		return 1;
	}

	for (int attempt = 0; attempt < FREEZE_RETRIES; attempt++) {
		if (read_event(name, "frozen", &frozen) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to read freezer state for %s: %s\n",
				name, strerror(errno));
			return 1;
		}
		if (frozen == freeze) {
			printf("tgbsctl: %s %s\n", freeze ? "paused" : "resumed", name);
			return 0;
		}
		usleep(FREEZE_RETRY_US);
	}

	fprintf(stderr, "tgbsctl: ERROR - timed out waiting to %s %s\n",
		operation, name);
	return 1;
}

static int write_cpu_runtime(const char *path, unsigned int cpu,
		unsigned long long value)
{
	char text[64];

	snprintf(text, sizeof(text), "%u %llu", cpu, value);
	return write_text(path, text);
}

static int restore_cpu_runtime(const char *path, unsigned int cpu,
		const struct runtime_config *previous)
{
	char text[64];

	for (size_t i = 0; i < previous->count; i++)
		if (previous->overrides[i].cpu == cpu)
			return write_cpu_runtime(path, cpu, previous->overrides[i].runtime);
	snprintf(text, sizeof(text), "%u default", cpu);
	return write_text(path, text);
}

static int set_runtime(const char *name, const char *value_text)
{
	char path[PATH_MAX];
	struct cpu_list possible = {0};
	struct runtime_config previous = {0}, actual = {0};
	unsigned long long *values = NULL;
	unsigned long long period, value;
	size_t *applied = NULL;
	size_t applied_count = 0;
	char *copy = NULL, *cursor, *token;
	int result = 1;

	/* A scalar retains the kernel's default-runtime write semantics. */
	if (strchr(value_text, ',') == NULL) {
		if (parse_runtime_value(value_text, &value) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - runtime must be a nonnegative integer or comma-separated list\n");
			return 2;
		}
	} else {
		if (read_possible_cpus(&possible) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to read possible CPUs: %s\n", strerror(errno));
			return 1;
		}
		values = calloc(possible.count, sizeof(*values));
		applied = calloc(possible.count, sizeof(*applied));
		copy = strdup(value_text);
		if (values == NULL || applied == NULL || copy == NULL) {
			fprintf(stderr, "tgbsctl: ERROR - unable to allocate runtime list: %s\n", strerror(errno));
			goto out;
		}
		cursor = copy;
		size_t count = 0;
		while ((token = strsep(&cursor, ",")) != NULL) {
			if (count >= possible.count ||
			    parse_runtime_value(token, &values[count]) != 0) {
				count = 0;
				break;
			}
			count++;
		}
		if (count != possible.count) {
			fprintf(stderr, "tgbsctl: ERROR - runtime list must contain exactly %zu nonnegative values, in possible-CPU order (including offline CPUs)\n",
				possible.count);
			result = 2;
			goto out;
		}
	}

	snprintf(path, sizeof(path), "%s/%s/cpu.period_us", CG_ROOT, name);
	if (read_u64(path, &period) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to read period for %s: %s\n", name, strerror(errno));
		goto out;
	}
	for (size_t i = 0; i < (values != NULL ? possible.count : 1); i++) {
		if ((values != NULL ? values[i] : value) > period) {
			fprintf(stderr, "tgbsctl: ERROR - runtime must be <= current period (%llu us)\n", period);
			goto out;
		}
	}
	snprintf(path, sizeof(path), "%s/%s/cpu.runtime_us", CG_ROOT, name);
	if (values == NULL) {
		if (write_u64(path, value) != 0 || read_runtime(path, &actual) != 0 ||
		    actual.default_runtime != value) {
			fprintf(stderr, "tgbsctl: ERROR - unable to set or verify runtime for %s\n", name);
			goto out;
		}
		printf("tgbsctl: set default runtime for %s to %llu us\n", name, value);
		result = 0;
		goto out;
	}
	if (read_runtime(path, &previous) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to read runtime for %s: %s\n", name, strerror(errno));
		goto out;
	}
	/* Release bandwidth before increasing reservations on other CPUs. Each
	 * kernel write updates one CPU; keep an undo order for partial failures. */
	for (int increasing = 0; increasing < 2; increasing++) {
		for (size_t i = 0; i < possible.count; i++) {
			unsigned int cpu = possible.cpus[i];

			if ((values[i] > runtime_for_cpu(&previous, cpu)) != increasing)
				continue;
			applied[applied_count++] = i;
			if (write_cpu_runtime(path, cpu, values[i]) != 0) {
				fprintf(stderr, "tgbsctl: ERROR - unable to set runtime for %s CPU %u: %s\n",
					name, cpu, strerror(errno));
				goto rollback;
			}
		}
	}
	if (read_runtime(path, &actual) != 0)
		goto verify_failed;
	for (size_t i = 0; i < possible.count; i++)
		if (runtime_for_cpu(&actual, possible.cpus[i]) != values[i])
			goto verify_failed;
	printf("tgbsctl: set per-CPU runtimes for %s to %s us\n", name, value_text);
	result = 0;
	goto out;

verify_failed:
	fprintf(stderr, "tgbsctl: ERROR - unable to verify per-CPU runtimes for %s\n", name);
rollback:
	while (applied_count > 0) {
		unsigned int cpu = possible.cpus[applied[--applied_count]];

		if (restore_cpu_runtime(path, cpu, &previous) != 0)
			fprintf(stderr, "tgbsctl: ERROR - unable to restore runtime for %s CPU %u: %s\n",
				name, cpu, strerror(errno));
	}
out:
	free(copy);
	free(applied);
	free(values);
	free_cpu_list(&possible);
	free_runtime(&previous);
	free_runtime(&actual);
	return result;
}

int cmd_set(const char *name, const char *field, const char *value_text)
{
	char path[PATH_MAX];
	char effective[CPU_LIST_SIZE];
	char limit[32];
	unsigned long long value;
	unsigned long long other;
	unsigned long long actual;
	const char *filename = NULL;
	int reclaim;

	if (require_domain(name) != 0)
		return 1;

	if (strcmp(field, "cpus") == 0) {
		if (configure_domain_cpus(name, value_text) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to set cpus for %s to '%s': %s\n",
				name, value_text, strerror(errno));
			return 1;
		}
		snprintf(path, sizeof(path), "%s/%s/cpuset.cpus.effective", CG_ROOT, name);
		if (read_text(path, effective, sizeof(effective)) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to verify effective cpus for %s: %s\n",
				name, strerror(errno));
			return 1;
		}
		printf("tgbsctl: set cpus for %s to %s (effective: %s)\n", name,
			strcmp(value_text, "inherit") == 0 ? "inherit" : value_text,
			effective);
		return 0;
	}

	if (strcmp(field, "reclaim") == 0) {
		if (parse_bool(value_text, &reclaim) != 0) {
			fprintf(stderr,
				"tgbsctl: ERROR - reclaim must be one of 0, 1, false, or true\n");
			return 2;
		}
		if (configure_domain_reclaim(name, reclaim) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to set reclaim for %s: %s\n",
				name, strerror(errno));
			return 1;
		}
		printf("tgbsctl: set reclaim for %s to %s\n",
			name, reclaim ? "true" : "false");
		return 0;
	}
	if (strcmp(field, "memory-max") == 0 ||
	    strcmp(field, "pids-max") == 0) {
		const char *limit_filename = strcmp(field, "memory-max") == 0 ?
			"memory.max" : "pids.max";

		if (parse_cgroup_limit(value_text, limit, sizeof(limit)) != 0) {
			fprintf(stderr,
				"tgbsctl: ERROR - %s must be a positive integer or max\n",
				field);
			return 2;
		}
		if (configure_domain_limit(name, limit_filename, limit) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to set %s for %s: %s\n",
				field, name, strerror(errno));
			return 1;
		}
		printf("tgbsctl: set %s for %s to %s\n", field, name, limit);
		return 0;
	}
	if (strcmp(field, "runtime") == 0 || strcmp(field, "runtime-us") == 0)
		return set_runtime(name, value_text);
	if (strcmp(field, "period") != 0) {
		fprintf(stderr,
			"tgbsctl: ERROR - set field must be 'runtime-us', 'runtime', 'period', 'cpus', "
			"'reclaim', 'memory-max', or 'pids-max'\n");
		return 2;
	}

	if (parse_positive(value_text, &value) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - VALUE_US must be a strictly positive integer\n");
		return 2;
	}

	struct runtime_config runtime;

	filename = "cpu.period_us";
	snprintf(path, sizeof(path), "%s/%s/cpu.runtime_us", CG_ROOT, name);
	if (read_runtime(path, &runtime) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to read runtime for %s: %s\n",
			name, strerror(errno));
		return 1;
	}
	other = max_runtime(&runtime);
	free_runtime(&runtime);
	if (value < other) {
		fprintf(stderr,
			"tgbsctl: ERROR - period must be >= current runtime (%llu us)\n",
			other);
		return 1;
	}

	snprintf(path, sizeof(path), "%s/%s/%s", CG_ROOT, name, filename);
	if (write_u64(path, value) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to set %s for %s: %s\n",
			field, name, strerror(errno));
		return 1;
	}
	if (read_u64(path, &actual) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to verify %s for %s\n",
			field, name);
		return 1;
	}
	if (actual != value) {
		fprintf(stderr, "tgbsctl: ERROR - unable to verify %s for %s\n",
			field, name);
		return 1;
	}

	printf("tgbsctl: set %s for %s to %llu us\n", field, name, value);
	return 0;
}
