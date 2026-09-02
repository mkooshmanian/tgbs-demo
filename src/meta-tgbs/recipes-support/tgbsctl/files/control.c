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

int cmd_set(const char *name, const char *field, const char *value_text)
{
	char path[PATH_MAX];
	unsigned long long value;
	unsigned long long other;
	unsigned long long actual;
	const char *filename;

	if (require_domain(name) != 0)
		return 1;
	if (parse_positive(value_text, &value) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - VALUE_US must be a strictly positive integer\n");
		return 2;
	}

	if (strcmp(field, "runtime") == 0) {
		filename = "cpu.runtime_us";
		snprintf(path, sizeof(path), "%s/%s/cpu.period_us", CG_ROOT, name);
		if (read_u64(path, &other) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to read period for %s: %s\n",
				name, strerror(errno));
			return 1;
		}
		if (value > other) {
			fprintf(stderr,
				"tgbsctl: ERROR - runtime must be <= current period (%llu us)\n",
				other);
			return 1;
		}
	} else if (strcmp(field, "period") == 0) {
		filename = "cpu.period_us";
		snprintf(path, sizeof(path), "%s/%s/cpu.runtime_us", CG_ROOT, name);
		if (read_u64(path, &other) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to read runtime for %s: %s\n",
				name, strerror(errno));
			return 1;
		}
		if (value < other) {
			fprintf(stderr,
				"tgbsctl: ERROR - period must be >= current runtime (%llu us)\n",
				other);
			return 1;
		}
	} else {
		fprintf(stderr, "tgbsctl: ERROR - set field must be 'runtime' or 'period'\n");
		return 2;
	}

	snprintf(path, sizeof(path), "%s/%s/%s", CG_ROOT, name, filename);
	if (write_u64(path, value) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to set %s for %s: %s\n",
			field, name, strerror(errno));
		return 1;
	}
	if (read_u64(path, &actual) != 0 || actual != value) {
		fprintf(stderr, "tgbsctl: ERROR - unable to verify %s for %s\n",
			field, name);
		return 1;
	}

	printf("tgbsctl: set %s for %s to %llu us\n", field, name, value);
	return 0;
}
