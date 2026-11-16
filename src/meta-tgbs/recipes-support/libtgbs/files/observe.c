/*
 * SPDX-License-Identifier: MIT
 *
 * Read-only observation of TGBS domains, extracted from tgbsctl list / inspect.
 *
 * cgroupfs is the source of truth for resource and task state. Volatile
 * main.pid markers record the leader PID and start time. This file returns
 * snapshots; the CLI retains responsibility for formatting and output.
 */

#define _GNU_SOURCE
#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_command(pid_t pid)
{
	char path[PATH_MAX];
	int fd;
	size_t used = 0;
	size_t capacity = 256;
	char *cmdline;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}

	cmdline = malloc(capacity + 1);
	if (cmdline == NULL) {
		close(fd);
		return NULL;
	}

	while (1) {
		ssize_t r;

		if (used == capacity) {
			size_t new_capacity = capacity * 2;
			char *larger;

			if (new_capacity <= capacity) {
				free(cmdline);
				close(fd);
				return NULL;
			}

			larger = realloc(cmdline, new_capacity + 1);
			if (larger == NULL) {
				free(cmdline);
				close(fd);
				return NULL;
			}

			cmdline = larger;
			capacity = new_capacity;
		}

		r = read(fd, cmdline + used, capacity - used);
		if (r < 0 && errno == EINTR) {
			continue;
		}

		if (r < 0) {
			free(cmdline);
			close(fd);
			return NULL;
		}

		if (r == 0) {
			break;
		}

		used += (size_t)r;
	}

	close(fd);
	if (used == 0) {
		free(cmdline);
		return NULL;
	}

	/* /proc/<pid>/cmdline stores argv entries separated by NUL bytes and
	 * normally ends with one. Join every entry for display; the shell quoting
	 * used to create argv is not retained by the kernel. */
	while (used > 0 && cmdline[used - 1] == '\0') {
		used--;
	}

	for (size_t i = 0; i < used; i++) {
		if (cmdline[i] == '\0') {
			cmdline[i] = ' ';
		}
	}

	cmdline[used] = '\0';
	return cmdline;
}

int read_main_meta(
	const char *run_root, const char *name, pid_t *pid, unsigned long long *starttime)
{
	char path[PATH_MAX], text[128], junk;
	int raw;

	snprintf(path, sizeof(path), "%s/%s/main.pid", run_root, name);
	if (read_text(path, text, sizeof(text)) != 0) {
		return -1;
	}

	if (sscanf(text, "%d %llu %c", &raw, starttime, &junk) != 2 || raw <= 0 || !*starttime) {
		errno = EPROTO;
		return -1;
	}

	*pid = raw;
	return 0;
}

static int read_event(const char *cgroup_root, const char *name, const char *key)
{
	char path[PATH_MAX], line[128], found[64];
	int value, result = -1;

	domain_path(cgroup_root, name, "cgroup.events", path);
	FILE *file = fopen(path, "re");

	if (file == NULL) {
		return -1;
	}
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "%63s %d", found, &value) == 2 && strcmp(found, key) == 0) {
			result = value;
			break;
		}
	}

	fclose(file);
	return result;
}

static int read_ids(const char *path, pid_t **ids, size_t *count)
{
	FILE *file = fopen(path, "re");
	int raw;

	*ids = NULL;
	*count = 0;
	if (file == NULL) {
		return -1;
	}

	int matched;

	while ((matched = fscanf(file, "%d", &raw)) == 1) {
		if (raw <= 0) {
			errno = EPROTO;
			goto fail;
		}

		pid_t *grown = reallocarray(*ids, *count + 1, sizeof(**ids));

		if (grown == NULL) {
			goto fail;
		}

		*ids = grown;
		(*ids)[(*count)++] = raw;
	}

	if (matched != EOF || ferror(file)) {
		errno = EPROTO;
		goto fail;
	}

	fclose(file);
	return 0;

fail:
	raw = errno;
	fclose(file);
	free(*ids);
	*ids = NULL;
	*count = 0;
	errno = raw;
	return -1;
}

static int read_value(const char *cgroup_root, const char *name, const char *file, uint64_t *value)
{
	char path[PATH_MAX];
	unsigned long long raw;
	domain_path(cgroup_root, name, file, path);
	if (read_u64(path, &raw) != 0) {
		return -1;
	}

	*value = raw;
	return 0;
}

static int read_limit(
	const char *cgroup_root, const char *name, const char *file, tgbs_limit_t *limit)
{
	char path[PATH_MAX], text[32];
	unsigned long long raw;
	domain_path(cgroup_root, name, file, path);
	if (read_text(path, text, sizeof(text)) != 0) {
		return -1;
	}

	if (strcmp(text, "max") == 0) {
		limit->unlimited = 1;
		return 0;
	}

	if (parse_runtime_value(text, &raw) != 0) {
		return -1;
	}

	limit->value = raw;
	return 0;
}

const char *tgbs_domain_state_name(tgbs_domain_state_t state)
{
	switch (state) {
	case TGBS_DOMAIN_UNKNOWN:
		return "unknown";
	case TGBS_DOMAIN_INITIALIZING:
		return "initializing";
	case TGBS_DOMAIN_RUNNING:
		return "running";
	case TGBS_DOMAIN_PAUSED:
		return "paused";
	case TGBS_DOMAIN_ORPHAN:
		return "orphan";
	case TGBS_DOMAIN_EXITED:
		return "exited";
	case TGBS_DOMAIN_STALE:
		return "stale";
	}

	return "unknown";
}

void tgbs_domain_info_free(tgbs_domain_info_t *info)
{
	if (info == NULL) {
		return;
	}

	free(info->cpu_budgets);
	free(info->processes);
	free(info->threads);
	free(info->command);
	free(info);
}

static int inspect_domain(
	const char *cgroup_root, const char *run_root, const char *name, tgbs_domain_info_t **out)
{
	char path[PATH_MAX];
	struct stat st;
	struct runtime_config config = { 0 };
	struct cpu_list effective = { 0 }, possible = { 0 };
	unsigned long long starttime;
	tgbs_domain_info_t *info;
	int saved_errno;

	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}

	*out = NULL;
	if (!valid_domain_name(name)) {
		errno = EINVAL;
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%s", run_root, name);
	if (lstat(path, &st) != 0) {
		return -1;
	}

	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}

	info = calloc(1, sizeof(*info));
	if (info == NULL) {
		return -1;
	}

	strcpy(info->name, name);
	info->state = TGBS_DOMAIN_INITIALIZING;
	domain_path(cgroup_root, name, "", path);
	int exists = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
	int populated = read_event(cgroup_root, name, "populated");

	if (read_main_meta(run_root, name, &info->main_pid, &starttime) == 0) {
		unsigned long long now;
		info->main_starttime = starttime;

		/* Without a verified leader, a populated cgroup is an orphan. A
		 * missing cgroup leaves stale runtime metadata. */
		if (!exists) {
			info->state = TGBS_DOMAIN_STALE;
		} else if (populated > 0) {
			info->state = TGBS_DOMAIN_ORPHAN;
		} else if (populated == 0) {
			info->state = TGBS_DOMAIN_EXITED;
		} else {
			info->state = TGBS_DOMAIN_UNKNOWN;
		}

		domain_path(cgroup_root, name, "cgroup.procs", path);
		if (read_ids(path, &info->processes, &info->process_count) != 0 &&
			errno == ENOMEM) {
			goto fail;
		}

		/* Match both /proc start time and cgroup membership to protect
		 * against PID reuse or a leader moved out of its domain. */
		int member = 0;

		for (size_t i = 0; i < info->process_count; i++) {
			if (info->processes[i] == info->main_pid) {
				member = 1;
			}
		}

		if (exists && member && read_starttime(info->main_pid, &now) == 0 &&
			now == starttime) {
			int frozen = read_event(cgroup_root, name, "frozen");

			if (frozen > 0) {
				info->state = TGBS_DOMAIN_PAUSED;
			} else if (frozen == 0) {
				info->state = TGBS_DOMAIN_RUNNING;
			} else {
				info->state = TGBS_DOMAIN_UNKNOWN;
			}

			info->command = read_command(info->main_pid);
		}
	} else if (errno != ENOENT) {
		info->state = TGBS_DOMAIN_UNKNOWN;
	} else if (!exists) {
		info->state = TGBS_DOMAIN_STALE;
	}

	if (info->processes == NULL) {
		domain_path(cgroup_root, name, "cgroup.procs", path);
		if (read_ids(path, &info->processes, &info->process_count) != 0 &&
			errno == ENOMEM) {
			goto fail;
		}
	}

	domain_path(cgroup_root, name, "cgroup.threads", path);
	if (read_ids(path, &info->threads, &info->thread_count) != 0 && errno == ENOMEM) {
		goto fail;
	}

	/* Read the temporal contract and its effective CPU selection. */
	domain_path(cgroup_root, name, "cpu.runtime_us", path);
	if (read_runtime(path, &config) == 0 &&
		read_value(cgroup_root, name, "cpu.period_us", &info->period_us) == 0 &&
		info->period_us) {
		info->default_runtime_us = config.default_runtime;
		info->available |= TGBS_INFO_CONTRACT;
	}

	domain_path(cgroup_root, name, "cpuset.cpus.effective", path);
	if (read_text(path, info->cpus, sizeof(info->cpus)) == 0 &&
		parse_cpu_list(info->cpus, &effective) == 0) {
		info->available |= TGBS_INFO_CPUS;
	}

	if ((info->available & (TGBS_INFO_CONTRACT | TGBS_INFO_CPUS)) ==
		(TGBS_INFO_CONTRACT | TGBS_INFO_CPUS)) {
		info->total_budget_percent =
			total_runtime(&config, &effective) * 100.0L / info->period_us;
	}

	/* Report possible CPUs, including offline ones; only effective CPUs
	 * contribute to the total budget above. */
	if ((info->available & TGBS_INFO_CONTRACT) && read_possible_cpus(&possible) == 0) {
		info->cpu_budgets = calloc(possible.count, sizeof(*info->cpu_budgets));
		if (info->cpu_budgets == NULL && possible.count) {
			goto fail;
		}

		info->cpu_count = possible.count;
		for (size_t i = 0; i < possible.count; i++) {
			info->cpu_budgets[i].cpu = possible.cpus[i];
			info->cpu_budgets[i].runtime_us =
				runtime_for_cpu(&config, possible.cpus[i]);
			for (size_t j = 0; j < effective.count; j++) {
				if (effective.cpus[j] == possible.cpus[i]) {
					info->cpu_budgets[i].enabled = 1;
				}
			}
		}
	}

	uint64_t reclaim;

	if (read_value(cgroup_root, name, "cpu.reclaim", &reclaim) == 0 && reclaim <= 1) {
		info->reclaim = reclaim;
		info->available |= TGBS_INFO_RECLAIM;
	}

	domain_path(cgroup_root, name, "cpu.stat", path);
	FILE *usage = fopen(path, "re");

	if (usage) {
		char line[256];
		unsigned long long raw;
		while (fgets(line, sizeof(line), usage)) {
			if (sscanf(line, "usage_usec %llu", &raw) == 1) {
				info->usage_us = raw;
				info->available |= TGBS_INFO_USAGE;
				break;
			}
		}

		fclose(usage);
	}

	/* Optional resource files may disappear during teardown. Keep the
	 * snapshot and mark only the measurements successfully obtained. */
	if (read_value(cgroup_root, name, "memory.current", &info->memory_current) == 0) {
		info->available |= TGBS_INFO_MEMORY_CURRENT;
	}

	if (read_value(cgroup_root, name, "memory.peak", &info->memory_peak) == 0) {
		info->available |= TGBS_INFO_MEMORY_PEAK;
	}

	if (read_limit(cgroup_root, name, "memory.max", &info->memory_max) == 0) {
		info->available |= TGBS_INFO_MEMORY_MAX;
	}

	if (read_value(cgroup_root, name, "pids.current", &info->pids_current) == 0) {
		info->available |= TGBS_INFO_PIDS_CURRENT;
	}

	if (read_limit(cgroup_root, name, "pids.max", &info->pids_max) == 0) {
		info->available |= TGBS_INFO_PIDS_MAX;
	}

	free_runtime(&config);
	free_cpu_list(&effective);
	free_cpu_list(&possible);
	*out = info;
	return 0;
fail:
	saved_errno = errno;

	free_runtime(&config);
	free_cpu_list(&effective);
	free_cpu_list(&possible);
	tgbs_domain_info_free(info);
	errno = saved_errno;
	return -1;
}

void tgbs_domain_list_free(tgbs_domain_info_t **domains, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		tgbs_domain_info_free(domains[i]);
	}

	free(domains);
}

int tgbs_domain_list_at(
	const char *cgroup_root, const char *run_root, tgbs_domain_info_t ***out, size_t *count)
{
	DIR *dir;
	struct dirent *entry;
	int saved_errno;

	if (out) {
		*out = NULL;
	}

	if (count) {
		*count = 0;
	}

	if (out == NULL || !count) {
		errno = EINVAL;
		return -1;
	}

	if (cgroup_root == NULL) {
		cgroup_root = CG_ROOT;
	}

	if (run_root == NULL) {
		run_root = RUN_ROOT;
	}

	if (cgroup_root[0] != '/' || run_root[0] != '/' || strlen(cgroup_root) >= PATH_MAX - 512 ||
		strlen(run_root) >= PATH_MAX - 512) {
		errno = EINVAL;
		return -1;
	}

	dir = opendir(run_root);
	if (dir == NULL) {
		return errno == ENOENT ? 0 : -1;
	}

	for (;;) {
		errno = 0;
		entry = readdir(dir);
		if (!entry) {
			if (errno) {
				goto fail;
			}

			break;
		}

		/* Ignore internal directories and names reserved for the runtime. */
		if (entry->d_name[0] == '.' || !valid_domain_name(entry->d_name)) {
			continue;
		}

		tgbs_domain_info_t *info;

		if (inspect_domain(cgroup_root, run_root, entry->d_name, &info) != 0) {
			if (errno == ENOENT || errno == ENOTDIR) {
				continue;
			}

			goto fail;
		}

		tgbs_domain_info_t **grown = reallocarray(*out, *count + 1, sizeof(**out));

		if (grown == NULL) {
			tgbs_domain_info_free(info);
			goto fail;
		}

		*out = grown;
		(*out)[(*count)++] = info;
	}

	closedir(dir);
	return 0;
fail:
	saved_errno = errno;

	closedir(dir);
	tgbs_domain_list_free(*out, *count);
	*out = NULL;
	*count = 0;
	errno = saved_errno;
	return -1;
}

int tgbs_domain_inspect(const char *name, tgbs_domain_info_t **info)
{
	return inspect_domain(CG_ROOT, RUN_ROOT, name, info);
}

int tgbs_domain_list(tgbs_domain_info_t ***domains, size_t *count)
{
	return tgbs_domain_list_at(CG_ROOT, RUN_ROOT, domains, count);
}
