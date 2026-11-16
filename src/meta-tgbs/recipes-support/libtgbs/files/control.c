/*
 * SPDX-License-Identifier: MIT
 *
 * Resource control for active TGBS domains, extracted from tgbsctl.
 *
 * Operations act directly on cgroupfs after checking runtime ownership and
 * readiness. The blocking run call remains responsible for domain cleanup.
 */

#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FREEZE_RETRIES 500
#define FREEZE_RETRY_US 10000

static int require_domain(const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	if (!valid_domain_name(name)) {
		errno = EINVAL;
		return -1;
	}

	/* Avoid mutating an unrelated cgroup that happens to have the same name. */
	snprintf(path, sizeof(path), "%s/%s", RUN_ROOT, name);
	if (stat(path, &st) != 0) {
		return -1;
	}

	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}

	/* Do not race run while it is configuring the cgroup and publishing
	 * its main-process marker. */
	snprintf(path, sizeof(path), "%s/%s/main.pid", RUN_ROOT, name);
	if (stat(path, &st) != 0) {
		if (errno == ENOENT) {
			errno = EAGAIN;
		}

		return -1;
	}

	if (!S_ISREG(st.st_mode)) {
		errno = EINVAL;
		return -1;
	}

	/* Runtime ownership alone does not prove the cgroup still exists. */
	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
	if (stat(path, &st) != 0) {
		return -1;
	}

	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}

	return 0;
}

int tgbs_domain_kill(const char *name)
{
	if (require_domain(name) != 0) {
		return -1;
	}

	return cgroup_kill(name);
}

static int freeze_domain(const char *name, int freeze)
{
	char path[PATH_MAX], line[128];

	if (require_domain(name) != 0) {
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%s/cgroup.freeze", CG_ROOT, name);
	if (write_u64(path, freeze) != 0) {
		return -1;
	}

	/* Wait for the kernel to confirm the requested freezer state. */
	snprintf(path, sizeof(path), "%s/%s/cgroup.events", CG_ROOT, name);
	for (int retry = 0; retry < FREEZE_RETRIES; retry++) {
		FILE *f = fopen(path, "re");

		if (f == NULL) {
			return -1;
		}

		int actual = -1;

		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "frozen %d", &actual) == 1) {
				break;
			}
		}

		int failed = ferror(f);

		fclose(f);
		if (failed) {
			errno = EIO;
			return -1;
		}

		if (actual == freeze) {
			return 0;
		}

		usleep(FREEZE_RETRY_US);
	}

	errno = ETIMEDOUT;
	return -1;
}

int tgbs_domain_pause(const char *name)
{
	return freeze_domain(name, 1);
}

int tgbs_domain_resume(const char *name)
{
	return freeze_domain(name, 0);
}

int tgbs_domain_set_runtime(const char *name, uint64_t value)
{
	char path[PATH_MAX];
	unsigned long long period;
	struct runtime_config actual = { 0 };

	if (require_domain(name) != 0) {
		return -1;
	}

	/* A scalar update changes the default budget, retaining CPU overrides. */
	domain_path(CG_ROOT, name, "cpu.period_us", path);
	if (read_u64(path, &period) != 0) {
		return -1;
	}

	if (value > period) {
		errno = EINVAL;
		return -1;
	}

	domain_path(CG_ROOT, name, "cpu.runtime_us", path);
	if (write_u64(path, value) != 0 || read_runtime(path, &actual) != 0) {
		return -1;
	}

	int ok = actual.default_runtime == value;

	free_runtime(&actual);
	if (!ok) {
		errno = EIO;
		return -1;
	}

	return 0;
}

static int write_cpu_runtime(const char *path, unsigned int cpu, uint64_t value)
{
	char text[64];

	snprintf(text, sizeof(text), "%u %llu", cpu, (unsigned long long)value);
	return write_text(path, text);
}

static int restore_cpu_runtime(
	const char *path, unsigned int cpu, const struct runtime_config *previous)
{
	char text[64];

	for (size_t i = 0; i < previous->count; i++) {
		if (previous->overrides[i].cpu == cpu) {
			return write_cpu_runtime(path, cpu, previous->overrides[i].runtime);
		}
	}

	snprintf(text, sizeof(text), "%u default", cpu);
	return write_text(path, text);
}

int tgbs_domain_set_cpu_runtimes(const char *name, const uint64_t *values, size_t count)
{
	char path[PATH_MAX];
	struct cpu_list possible = { 0 };
	struct runtime_config previous = { 0 }, actual = { 0 };
	size_t *applied = NULL, applied_count = 0;
	unsigned long long period;
	int failure = 0;

	if (values == NULL || !count) {
		errno = EINVAL;
		return -1;
	}

	if (require_domain(name) != 0 || read_possible_cpus(&possible) != 0) {
		return -1;
	}

	if (count != possible.count) {
		errno = EINVAL;
		goto fail;
	}

	domain_path(CG_ROOT, name, "cpu.period_us", path);
	if (read_u64(path, &period) != 0) {
		goto fail;
	}

	for (size_t i = 0; i < count; i++) {
		if (values[i] > period) {
			errno = EINVAL;
			goto fail;
		}
	}

	applied = calloc(count, sizeof(*applied));
	if (!applied) {
		goto fail;
	}

	domain_path(CG_ROOT, name, "cpu.runtime_us", path);
	if (read_runtime(path, &previous) != 0) {
		goto fail;
	}

	/* Release bandwidth before increasing reservations on other CPUs. Each
	 * kernel write updates one CPU; keep an undo order for partial failures. */
	for (int increasing = 0; increasing < 2; increasing++) {
		for (size_t i = 0; i < count; i++) {
			unsigned int cpu = possible.cpus[i];

			if ((values[i] > runtime_for_cpu(&previous, cpu)) != increasing) {
				continue;
			}

			applied[applied_count++] = i;
			if (write_cpu_runtime(path, cpu, values[i]) != 0) {
				goto rollback;
			}
		}
	}

	if (read_runtime(path, &actual) != 0) {
		goto rollback;
	}

	for (size_t i = 0; i < count; i++) {
		if (runtime_for_cpu(&actual, possible.cpus[i]) != values[i]) {
			errno = EIO;
			goto rollback;
		}
	}

	goto out;

rollback:

	/* Retain the original error while restoring already-applied CPU budgets. */
	failure = errno;
	while (applied_count) {
		restore_cpu_runtime(path, possible.cpus[applied[--applied_count]], &previous);
	}

	goto out;

fail:
	failure = errno;

out:
	free(applied);
	free_cpu_list(&possible);
	free_runtime(&previous);
	free_runtime(&actual);
	if (failure) {
		errno = failure;
		return -1;
	}

	return 0;
}

int tgbs_domain_set_period(const char *name, uint64_t value)
{
	char path[PATH_MAX];
	struct runtime_config config = { 0 };
	unsigned long long actual;
	if (!value) {
		errno = EINVAL;
		return -1;
	}

	if (require_domain(name) != 0) {
		return -1;
	}

	/* The new period must cover both the default and all CPU overrides. */
	domain_path(CG_ROOT, name, "cpu.runtime_us", path);
	if (read_runtime(path, &config) != 0) {
		return -1;
	}

	unsigned long long maximum = max_runtime(&config);
	free_runtime(&config);
	if (value < maximum) {
		errno = EINVAL;
		return -1;
	}

	domain_path(CG_ROOT, name, "cpu.period_us", path);
	if (write_u64(path, value) != 0 || read_u64(path, &actual) != 0) {
		return -1;
	}

	if (actual != value) {
		errno = EIO;
		return -1;
	}

	return 0;
}

int tgbs_domain_set_cpus(const char *name, const char *cpus)
{
	struct cpu_list list = { 0 };

	if (cpus == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (strcmp(cpus, "inherit") != 0) {
		if (parse_cpu_list(cpus, &list) != 0) {
			return -1;
		}

		free_cpu_list(&list);
	}

	if (require_domain(name) != 0) {
		return -1;
	}

	return configure_domain_cpus(name, cpus);
}

int tgbs_domain_set_reclaim(const char *name, int reclaim)
{
	if (reclaim != 0 && reclaim != 1) {
		errno = EINVAL;
		return -1;
	}

	if (require_domain(name) != 0) {
		return -1;
	}

	return configure_domain_reclaim(name, reclaim);
}

static int set_limit(const char *name, const char *file, tgbs_limit_t limit)
{
	char text[32];

	if (limit_text(limit, text) != 0 || require_domain(name) != 0) {
		return -1;
	}

	return configure_domain_limit(name, file, text);
}

int tgbs_domain_set_memory_max(const char *name, tgbs_limit_t limit)
{
	return set_limit(name, "memory.max", limit);
}

int tgbs_domain_set_pids_max(const char *name, tgbs_limit_t limit)
{
	return set_limit(name, "pids.max", limit);
}
