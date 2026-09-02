/*
 * tgbsctl list / inspect -- read-only observation of active TGBS domains.
 *
 * cgroupfs under CG_ROOT is the single source of truth for resource and task
 * state. /run/tgbs only holds a volatile ownership marker (main.pid) that the
 * kernel cannot rebuild. Nothing here mutates the system.
 */

#include "tgbsctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>

static int read_u64_file(const char *path, unsigned long long *out)
{
	char buf[64];
	int fd = open(path, O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = '\0';

	errno = 0;
	unsigned long long value = strtoull(buf, NULL, 10);
	if (errno != 0 || value == 0)
		return -1;
	*out = value;
	return 0;
}

static int read_populated(const char *name, int *out)
{
	char path[PATH_MAX];
	char buf[64];
	int fd;
	ssize_t r;

	snprintf(path, sizeof(path), "%s/%s/cgroup.events", CG_ROOT, name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = '\0';

	/* cgroup.events is "key value\n" lines. Walk each newline-terminated
	 * field, advancing past the delimiter once it has been null-terminated. */
	char *start = buf;
	for (char *nl = buf; (nl = strpbrk(nl, "\n")) != NULL; nl++) {
		*nl = '\0';
		if (strncmp(start, "populated", 9) == 0) {
			long v = strtol(start + 9, NULL, 10);
			if (v < 0)
				v = 0;
			*out = v;
			return 0;
		}
		start = nl + 1;
	}
	*out = 0;
	return 0;
}

static int read_procs(const char *name, char *out, size_t outsz)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "%s/%s/cgroup.procs", CG_ROOT, name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	out[0] = '\0';
	size_t used = 0;
	while (1) {
		char pidbuf[32];
		ssize_t n = read(fd, pidbuf, sizeof(pidbuf) - 1);
		if (n <= 0)
			break;
		pidbuf[n] = '\0';
		for (char *p = pidbuf; *p != '\0'; p++) {
			if (!isdigit((unsigned char) *p))
				continue;
			pid_t pid = (pid_t) strtoul(p, &p, 10);
			if (pid <= 0)
				continue;
			int len = snprintf(out + used, outsz - used, "%s%d",
				used > 0 ? " " : "", (int) pid);
			if (len < 0 || (size_t) len >= outsz - used)
				break;
			used += len;
		}
	}
	return 0;
}

static char *read_command(pid_t pid)
{
	char path[PATH_MAX];
	int fd;
	size_t used = 0;
	size_t capacity = 256;
	char *cmdline;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int) pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	cmdline = malloc(capacity);
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
			larger = realloc(cmdline, new_capacity);
			if (larger == NULL) {
				free(cmdline);
				close(fd);
				return NULL;
			}
			cmdline = larger;
			capacity = new_capacity;
		}

		r = read(fd, cmdline + used, capacity - used);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			free(cmdline);
			close(fd);
			return NULL;
		}
		if (r == 0)
			break;
		used += (size_t) r;
	}
	close(fd);
	if (used == 0) {
		free(cmdline);
		return NULL;
	}

	/* /proc/<pid>/cmdline stores argv entries separated by NUL bytes and
	 * normally ends with one. Join every entry for display; the shell quoting
	 * used to create argv is not retained by the kernel. */
	while (used > 0 && cmdline[used - 1] == '\0')
		used--;
	for (size_t i = 0; i < used; i++) {
		if (cmdline[i] == '\0')
			cmdline[i] = ' ';
	}
	cmdline[used] = '\0';
	return cmdline;
}

static int read_config(const char *name, unsigned long long *runtime,
		unsigned long long *period)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/cpu.runtime_us", CG_ROOT, name);
	if (read_u64_file(path, runtime) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/%s/cpu.period_us", CG_ROOT, name);
	if (read_u64_file(path, period) != 0)
		return -1;
	return 0;
}

static int read_main_meta(const char *name, pid_t *pid,
		unsigned long long *starttime)
{
	char path[PATH_MAX];
	char buf[128];
	int fd;
	ssize_t r;

	snprintf(path, sizeof(path), "%s/%s/main.pid", RUN_ROOT, name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = '\0';

	if (sscanf(buf, "%d %llu", (int *)pid, starttime) != 2)
		return -1;
	if (*pid <= 0 || *starttime == 0)
		return -1;
	return 0;
}

/* A main process is alive when /proc/<pid> exists, its starttime matches the
 * recorded one (guards PID reuse), and it is still tracked by the cgroup. */
static int main_is_alive(const char *name, pid_t pid, unsigned long long starttime)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/proc/%d", (int) pid);
	if (access(path, F_OK) != 0)
		return 0;

	unsigned long long now_start;
	if (read_starttime(pid, &now_start) != 0)
		return 0;
	if (now_start != starttime)
		return 0;

	int populated = -1;
	read_populated(name, &populated);
	return populated == 1;
}

static enum domain_state domain_state(const char *name, pid_t pid,
		unsigned long long starttime)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
	if (access(path, F_OK) != 0)
		return STATE_STALE;

	int populated = 0;
	read_populated(name, &populated);

	if (main_is_alive(name, pid, starttime))
		return STATE_RUNNING;
	return populated ? STATE_ORPHAN : STATE_EXITED;
}

const char *state_name(enum domain_state s)
{
	switch (s) {
	case STATE_UNKNOWN: return "unknown";
	case STATE_INITIALIZING: return "initializing";
	case STATE_RUNNING: return "running";
	case STATE_ORPHAN: return "orphan";
	case STATE_EXITED: return "exited";
	case STATE_STALE: return "stale";
	}
	return "?";
}

int cmd_list(void)
{
	DIR *dir = opendir(RUN_ROOT);
	if (dir == NULL)
		return 0;

	printf("%-24s %-12s %-8s %-12s %-12s\n",
		"NAME", "STATE", "PID", "RUNTIME_US", "PERIOD_US");

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		pid_t pid = 0;
		unsigned long long starttime = 0;
		if (read_main_meta(ent->d_name, &pid, &starttime) != 0) {
			printf("%-24s %-12s %-8s %-12s %-12s\n",
				ent->d_name, "initializing", "-", "-", "-");
			continue;
		}

		enum domain_state st = domain_state(ent->d_name, pid, starttime);
		unsigned long long runtime = 0, period = 0;
		int cfg = read_config(ent->d_name, &runtime, &period);

		printf("%-24s %-12s %-8lld %-12lld %-12lld\n",
			ent->d_name, state_name(st),
			(long long) pid,
			cfg == 0 ? (long long) runtime : -1,
			cfg == 0 ? (long long) period : -1);
	}
	closedir(dir);
	return 0;
}

int cmd_inspect(const char *name)
{
	if (!is_valid_name(name)) {
		fprintf(stderr, "tgbsctl: ERROR - --name must match [a-zA-Z0-9_.-]+ (max 255, no '/'); got '%s'\n",
			name);
		return 2;
	}

	pid_t pid = 0;
	unsigned long long starttime = 0;
	if (read_main_meta(name, &pid, &starttime) != 0) {
		printf("Name:        %s\n", name);
		printf("State:       unknown\n");
		fprintf(stderr, "tgbsctl: ERROR - no runtime metadata for %s\n", name);
		return 1;
	}

	enum domain_state st = domain_state(name, pid, starttime);

	unsigned long long runtime = 0, period = 0;
	int cfg = read_config(name, &runtime, &period);

	char procs[2048] = "";
	read_procs(name, procs, sizeof(procs));

	char *command = NULL;
	if (main_is_alive(name, pid, starttime))
		command = read_command(pid);

	printf("Name:        %s\n", name);
	printf("State:       %s\n", state_name(st));
	printf("Main PID:    %d\n", (int) pid);
	printf("Runtime:     %lld us\n", cfg == 0 ? (long long) runtime : -1);
	printf("Period:      %lld us\n", cfg == 0 ? (long long) period : -1);
	printf("Processes:   %s\n", procs[0] ? procs : "(none)");
	printf("Command:     %s\n", command != NULL ? command : "(not alive)");
	free(command);
	return 0;
}
