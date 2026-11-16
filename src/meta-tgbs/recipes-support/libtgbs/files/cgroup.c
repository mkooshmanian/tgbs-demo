/*
 * SPDX-License-Identifier: MIT
 *
 * Shared cgroupfs and process-identity helpers extracted from tgbsctl.
 *
 * Validate names and limits, read and write cgroup controls, verify the host
 * environment, and terminate remaining domain tasks. Errors are returned to
 * the caller without printing diagnostics.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <linux/magic.h>
#include <sys/vfs.h>
#include <sys/types.h>

#include "internal.h"

int is_valid_name(const char *name)
{
	size_t i;

	if (name == NULL || name[0] == '\0' || strlen(name) > 255 || strcmp(name, ".") == 0 ||
		strcmp(name, "..") == 0) {
		return 0;
	}

	for (i = 0; name[i] != '\0'; i++) {
		char c = name[i];

		if (!isalnum((unsigned char)c) && c != '_' && c != '.' && c != '-') {
			return 0;
		}
	}

	return 1;
}

/* Parse a strictly positive unsigned long long, rejecting trailing junk. */
int parse_positive(const char *text, unsigned long long *out)
{
	char *end;

	if (text == NULL || out == NULL) {
		errno = EINVAL;
		return -1;
	}

	errno = 0;
	unsigned long long value = strtoull(text, &end, 10);
	if (text[0] < '0' || text[0] > '9' || errno != 0 || end == text || *end != '\0') {
		return -1;
	}

	if (value == 0) {
		return -1;
	}

	*out = value;
	return 0;
}

int parse_cgroup_limit(const char *text, char *out, size_t outsz)
{
	unsigned long long value;
	int len;

	if (text == NULL || out == NULL || outsz == 0) {
		errno = EINVAL;
		return -1;
	}

	if (strcmp(text, "max") == 0) {
		if (outsz < sizeof("max")) {
			errno = ENOSPC;
			return -1;
		}

		memcpy(out, "max", sizeof("max"));
		return 0;
	}

	if (parse_positive(text, &value) != 0) {
		errno = EINVAL;
		return -1;
	}

	len = snprintf(out, outsz, "%llu", value);
	if (len < 0 || (size_t)len >= outsz) {
		errno = ENOSPC;
		return -1;
	}

	return 0;
}

int read_starttime(pid_t pid, unsigned long long *out)
{
	char path[PATH_MAX];
	char buf[512];
	int fd;
	ssize_t r;
	char *after;
	unsigned long long value;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}

	do {
		r = read(fd, buf, sizeof(buf) - 1);
	} while (r < 0 && errno == EINTR);
	int saved_errno = errno;

	close(fd);
	if (r <= 0) {
		errno = r < 0 ? saved_errno : EPROTO;
		return -1;
	}

	buf[r] = '\0';

	/* comm is the 2nd field and is enclosed in parentheses; it may itself
	 * contain spaces or ')', so the last ')' is the true delimiter. */
	after = strrchr(buf, ')');
	if (after == NULL) {
		return -1;
	}

	after++;

	/* after points at field 3 (state). starttime is field 22 overall, so it
	 * is the 20th token after the state. Skip the state (a single char) and
	 * the 18 numeric fields 4..21 that precede it, then read starttime. Fields
	 * 3..21 are skipped without conversion to tolerate the signed ones. */
	char *p = after;
	char *end = NULL;
	int field = 3;

	while (field < 22) {
		while (*p == ' ' || *p == '\t') {
			p++;
		}

		if (*p == '\0') {
			return -1;
		}
		while (*p != '\0' && *p != ' ' && *p != '\t') {
			p++;
		}

		field++;
	}
	while (*p == ' ' || *p == '\t') {
		p++;
	}

	errno = 0;
	value = strtoull(p, &end, 10);
	if (end == p || errno != 0) {
		return -1;
	}

	if (value == 0) {
		return -1;
	}

	*out = value;
	return 0;
}

/* Read back a cgroup file and confirm the value matches, so a silent kernel
 * write cannot leave a cgroup configured with a stale value. */
int read_u64(const char *path, unsigned long long *out)
{
	char buf[32];
	char *end;
	int fd;
	ssize_t r;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}

	do {
		r = read(fd, buf, sizeof(buf) - 1);
	} while (r < 0 && errno == EINTR);
	int saved_errno = errno;

	close(fd);
	if (r <= 0) {
		errno = r < 0 ? saved_errno : EPROTO;
		return -1;
	}

	buf[r] = '\0';

	errno = 0;
	unsigned long long value = strtoull(buf, &end, 10);
	if (buf[0] < '0' || buf[0] > '9' || errno != 0 || end == buf) {
		if (!errno) {
			errno = EINVAL;
		}

		return -1;
	}
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
		end++;
	}

	if (*end != '\0') {
		errno = EINVAL;
		return -1;
	}

	*out = value;
	return 0;
}

int write_text(const char *path, const char *value)
{
	const char newline = '\n';
	const char *data = value;
	size_t len = strlen(value);
	int fd;
	ssize_t r;

	/* A zero-length write is a no-op. cgroupfs uses a blank line to clear a
	 * cpuset request and restore inheritance from the parent. */
	if (len == 0) {
		data = &newline;
		len = 1;
	}

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}

	do {
		r = write(fd, data, len);
	} while (r < 0 && errno == EINTR);
	if (r != (ssize_t)len) {
		int saved_errno = r < 0 ? errno : EIO;

		close(fd);
		errno = saved_errno;
		return -1;
	}

	close(fd);
	return 0;
}

int read_text(const char *path, char *out, size_t outsz)
{
	int fd;
	ssize_t r;

	if (outsz < 2) {
		errno = EINVAL;
		return -1;
	}

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}

	do {
		r = read(fd, out, outsz - 1);
	} while (r < 0 && errno == EINTR);
	close(fd);
	if (r < 0) {
		return -1;
	}

	out[r] = '\0';

	while (r > 0 &&
		(out[r - 1] == '\n' || out[r - 1] == '\r' || out[r - 1] == ' ' ||
			out[r - 1] == '\t')) {
		out[--r] = '\0';
	}

	return 0;
}

int configure_domain_cpus(const char *name, const char *cpu_list)
{
	char path[PATH_MAX];
	char effective[CPU_LIST_SIZE];
	char inherited[CPU_LIST_SIZE];
	char previous[CPU_LIST_SIZE];
	const char *requested;
	int saved_errno;

	if (cpu_list == NULL || cpu_list[0] == '\0') {
		errno = EINVAL;
		return -1;
	}

	requested = cpu_list;
	if (strcmp(cpu_list, "inherit") == 0) {

		/* A populated cpuset cannot always be cleared back to an empty,
		 * inheriting request. Resolve the root's current effective list so
		 * the operation is reliable for both new and running domains. */
		snprintf(path, sizeof(path), "%s/cpuset.cpus.effective", CG_ROOT);
		if (read_text(path, inherited, sizeof(inherited)) != 0 || inherited[0] == '\0') {
			if (errno == 0) {
				errno = EINVAL;
			}

			return -1;
		}

		requested = inherited;
	}

	snprintf(path, sizeof(path), "%s/%s/cpuset.cpus", CG_ROOT, name);
	if (read_text(path, previous, sizeof(previous)) != 0) {
		return -1;
	}

	if (write_text(path, requested) != 0) {
		return -1;
	}

	/* The requested list may be filtered by the parent's effective cpuset or
	 * by CPU hotplug. Verify that it still leaves at least one usable CPU. */
	snprintf(path, sizeof(path), "%s/%s/cpuset.cpus.effective", CG_ROOT, name);
	if (read_text(path, effective, sizeof(effective)) == 0 && effective[0] != '\0') {
		return 0;
	}

	saved_errno = errno != 0 ? errno : EINVAL;
	snprintf(path, sizeof(path), "%s/%s/cpuset.cpus", CG_ROOT, name);
	write_text(path, previous);
	errno = saved_errno;
	return -1;
}

int configure_domain_reclaim(const char *name, int reclaim)
{
	char path[PATH_MAX];
	unsigned long long actual;

	snprintf(path, sizeof(path), "%s/%s/cpu.reclaim", CG_ROOT, name);
	if (write_u64(path, reclaim ? 1 : 0) != 0) {
		return -1;
	}

	if (read_u64(path, &actual) != 0) {
		return -1;
	}

	if (actual != (unsigned long long)(reclaim ? 1 : 0)) {
		errno = EIO;
		return -1;
	}

	return 0;
}

int configure_domain_limit(const char *name, const char *filename, const char *value)
{
	char path[PATH_MAX];
	char normalized[32];
	char actual[32];

	if (strcmp(filename, "memory.max") != 0 && strcmp(filename, "pids.max") != 0) {
		errno = EINVAL;
		return -1;
	}

	if (parse_cgroup_limit(value, normalized, sizeof(normalized)) != 0) {
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%s/%s", CG_ROOT, name, filename);
	if (write_text(path, normalized) != 0) {
		return -1;
	}

	if (read_text(path, actual, sizeof(actual)) != 0) {
		return -1;
	}

	if (strcmp(actual, normalized) != 0) {
		errno = EIO;
		return -1;
	}

	return 0;
}

/* Terminate every remaining task inside the cgroup. */
int cgroup_kill(const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s/cgroup.kill", CG_ROOT, name);
	if (write_u64(path, 1) == 0) {
		return 0;
	}

	/* Either the kernel does not support cgroup.kill, or it returned an
	 * error (e.g. empty cgroup). Fall back to killing the remaining pids. */
	char procs_path[PATH_MAX];

	snprintf(procs_path, sizeof(procs_path), "%s/%s/cgroup.procs", CG_ROOT, name);
	FILE *f = fopen(procs_path, "r");

	if (f == NULL) {
		return -1;
	}

	int failed = 0;
	int saved_errno = 0;
	int raw_pid;

	while (fscanf(f, "%d", &raw_pid) == 1) {
		if (raw_pid > 0 && kill((pid_t)raw_pid, SIGKILL) != 0 && errno != ESRCH) {
			failed = 1;
			saved_errno = errno;
		}
	}

	fclose(f);
	if (failed) {
		errno = saved_errno;
		return -1;
	}

	return 0;
}

int write_u64(const char *path, unsigned long long value)
{
	char text[32];

	snprintf(text, sizeof(text), "%llu", value);
	return write_text(path, text);
}

int valid_domain_name(const char *name)
{
	return is_valid_name(name) && strcmp(name, "channels") != 0 &&
		strcmp(name, "rootfs") != 0 &&
		strcmp(tgbs_contract_canonical_domain(name), HOST_PARTICIPANT) != 0;
}

/* cgroup v2 must be mounted as type cgroup2 on /sys/fs/cgroup. */
int verify_cgroup_env(void)
{
	struct statfs fs;
	char controllers[512];
	const char *required[] = { "cpu", "cpuset", "memory", "pids" };

	if (statfs(CG_ROOT, &fs) != 0) {
		return -1;
	}

	if ((unsigned long)fs.f_type != CGROUP2_SUPER_MAGIC) {
		errno = ENODEV;
		return -1;
	}

	if (read_text(CG_ROOT "/cgroup.controllers", controllers, sizeof(controllers)) != 0) {
		return -1;
	}

	for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
		char copy[512], *save, *word;
		int found = 0;

		strcpy(copy, controllers);
		for (word = strtok_r(copy, " \t\n", &save); word;
			word = strtok_r(NULL, " \t\n", &save)) {
			if (strcmp(word, required[i]) == 0) {
				found = 1;
			}
		}

		if (!found) {
			errno = ENODEV;
			return -1;
		}
	}

	return 0;
}

int limit_text(tgbs_limit_t limit, char text[32])
{
	if (limit.unlimited == 1) {
		strcpy(text, "max");
		return 0;
	}

	if (limit.unlimited != 0 || limit.value == 0) {
		errno = EINVAL;
		return -1;
	}

	snprintf(text, 32, "%llu", (unsigned long long)limit.value);
	return 0;
}

int domain_path(const char *cgroup_root, const char *name, const char *file, char path[PATH_MAX])
{
	int n = snprintf(path, PATH_MAX, "%s/%s/%s", cgroup_root, name, file);

	if (n < 0 || n >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}

	return 0;
}
