/*
 * SPDX-License-Identifier: MIT
 *
 * tgbsctl - minimal daemonless TGBS runtime and inspection tool
 *
 * The "run" command creates a TGBS cgroup directly under /sys/fs/cgroup,
 * configures its temporal contract, and starts a command as PID 1 inside new
 * PID, mount, UTS, IPC, and cgroup namespaces. A volatile marker under /run/tgbs
 * records the main process identity so that "list" and "inspect" can correlate
 * userspace processes with their TGBS cgroup. The channel commands manage
 * immutable communication contracts below /run/tgbs/channels.
 *
 * The cgroup lifetime is tied to the main process. When it exits, tgbsctl
 * terminates any remaining tasks, then removes the cgroup and its marker.
 * Observation derives domain state from cgroupfs, /proc, and the marker.
 * Control commands act directly on cgroupfs and the volatile channel store;
 * no daemon or persistent state database is involved.
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

#include "tgbsctl.h"

void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s run --name NAME [--hostname HOSTNAME]\n"
		"         --runtime-us RUNTIME --period-us PERIOD\n"
		"         [--cpus CPU-LIST|inherit] [--reclaim BOOL]\n"
		"         [--memory-max BYTES|max] [--pids-max COUNT|max]\n"
		"         COMMAND [ARGS...]\n"
		"  %s list\n"
		"  %s inspect NAME\n"
		"  %s kill NAME\n"
		"  %s pause NAME\n"
		"  %s resume NAME\n"
		"  %s set NAME runtime VALUE_US\n"
		"  %s set NAME period VALUE_US\n"
		"  %s set NAME cpus CPU-LIST|inherit\n"
		"  %s set NAME reclaim 0|1|false|true\n"
		"  %s set NAME memory-max BYTES|max\n"
		"  %s set NAME pids-max COUNT|max\n"
		"  %s channel create --name NAME --source DOMAIN --destination DOMAIN\n"
		"         --max-message-size BYTES --type queuing|sampling\n"
		"         [--destination DOMAIN ...] [--refresh-period-us PERIOD]\n"
		"  %s channel list\n"
		"  %s channel inspect NAME\n"
		"  %s channel delete NAME\n"
		"\n"
		"Commands:\n"
		"  run      Run COMMAND as PID 1 in isolated PID, mount, UTS, IPC, and\n"
		"           cgroup namespaces, with an ephemeral overlay rootfs and\n"
		"           private /tmp and /run mounts,\n"
		"           in a TGBS cgroup named NAME under %s.\n"
		"           Run options must follow 'run' and precede COMMAND.\n"
		"           RUNTIME and PERIOD are in microseconds and must satisfy\n"
		"           0 < RUNTIME <= PERIOD.\n"
		"           CPU-LIST uses the cpuset list syntax, for example 0-1 or 0,2;\n"
		"           inherit selects the cgroup root's effective CPU list.\n"
		"           BOOL accepts 0, 1, false, or true.\n"
		"           Resource limits are positive integers or max (the default).\n"
		"           HOSTNAME is set in the private UTS namespace before privileges\n"
		"           are reduced. CAP_SYS_ADMIN is unavailable to COMMAND.\n"
		"  list     List the TGBS domains known to this runtime.\n"
		"  inspect  Show the state, temporal contract, and processes for NAME.\n"
		"  kill     Kill every process in NAME. The run supervisor then cleans up.\n"
		"  pause    Freeze every process in NAME.\n"
		"  resume   Unfreeze every process in NAME.\n"
		"  set      Change one value of NAME's temporal contract.\n"
		"  channel  Manage immutable inter-container channel contracts.\n"
		"           TYPE is required: queuing or sampling.\n"
		"           Queuing requires exactly one destination.\n"
		"           Sampling requires REFRESH_PERIOD in microseconds and allows\n"
		"           up to 64 distinct destinations.\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog, prog, prog, prog, prog, CG_ROOT);
}

int is_valid_name(const char *name)
{
	size_t i;

	if (name == NULL || name[0] == '\0' || strlen(name) > 255 ||
	    strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return 0;
	for (i = 0; name[i] != '\0'; i++) {
		char c = name[i];
		if (!isalnum((unsigned char) c) && c != '_' && c != '.' && c != '-')
			return 0;
	}
	return 1;
}

/* Parse a strictly positive unsigned long long, rejecting trailing junk. */
int parse_positive(const char *text, unsigned long long *out)
{
	char *end;
	errno = 0;
	unsigned long long value = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return -1;
	if (value == 0)
		return -1;
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

int parse_bool(const char *text, int *out)
{
	if (strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0) {
		*out = 1;
		return 0;
	}
	if (strcmp(text, "0") == 0 || strcasecmp(text, "false") == 0) {
		*out = 0;
		return 0;
	}
	return -1;
}

int write_u64(const char *path, unsigned long long value)
{
	char buf[32];
	int len;
	int fd;
	ssize_t r;

	len = snprintf(buf, sizeof(buf), "%llu", (unsigned long long) value);
	if (len < 0 || len >= (int) sizeof(buf))
		return -1;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	r = write(fd, buf, (size_t) len);
	close(fd);

	if (r != (ssize_t) len)
		return -1;
	return 0;
}

/* Read the starttime (field 22, kernel ticks since boot) from
 * /proc/<pid>/stat. The comm field (field 2) may contain spaces and
 * parentheses, so we split on the last ')' instead of the first. */
int read_starttime(pid_t pid, unsigned long long *out)
{
	char path[PATH_MAX];
	char buf[512];
	int fd;
	ssize_t r;
	char *after;
	unsigned long long value;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = '\0';

	/* comm is the 2nd field and is enclosed in parentheses; it may itself
	 * contain spaces or ')', so the last ')' is the true delimiter. */
	after = strrchr(buf, ')');
	if (after == NULL)
		return -1;
	after++;

	/* after points at field 3 (state). starttime is field 22 overall, so it
	 * is the 20th token after the state. Skip the state (a single char) and
	 * the 18 numeric fields 4..21 that precede it, then read starttime. Fields
	 * 3..21 are skipped without conversion to tolerate the signed ones. */
	char *p = after;
	char *end = NULL;
	int field = 3;
	while (field < 22) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			return -1;
		while (*p != '\0' && *p != ' ' && *p != '\t')
			p++;
		field++;
	}
	while (*p == ' ' || *p == '\t')
		p++;
	errno = 0;
	value = strtoull(p, &end, 10);
	if (end == p || errno != 0)
		return -1;

	if (value == 0)
		return -1;
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

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = '\0';

	errno = 0;
	unsigned long long value = strtoull(buf, &end, 10);
	if (errno != 0 || end == buf)
		return -1;
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
		end++;
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

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
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

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	do {
		r = read(fd, out, outsz - 1);
	} while (r < 0 && errno == EINTR);
	close(fd);
	if (r < 0)
		return -1;
	out[r] = '\0';

	while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == '\r' ||
			  out[r - 1] == ' ' || out[r - 1] == '\t'))
		out[--r] = '\0';
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
		if (read_text(path, inherited, sizeof(inherited)) != 0 ||
		    inherited[0] == '\0') {
			if (errno == 0)
				errno = EINVAL;
			return -1;
		}
		requested = inherited;
	}

	snprintf(path, sizeof(path), "%s/%s/cpuset.cpus", CG_ROOT, name);
	if (read_text(path, previous, sizeof(previous)) != 0)
		return -1;
	if (write_text(path, requested) != 0)
		return -1;

	/* The requested list may be filtered by the parent's effective cpuset or
	 * by CPU hotplug. Verify that it still leaves at least one usable CPU. */
	snprintf(path, sizeof(path), "%s/%s/cpuset.cpus.effective", CG_ROOT, name);
	if (read_text(path, effective, sizeof(effective)) == 0 && effective[0] != '\0')
		return 0;

	saved_errno = errno != 0 ? errno : EINVAL;
	snprintf(path, sizeof(path), "%s/%s/cpuset.cpus", CG_ROOT, name);
	if (write_text(path, previous) != 0)
		fprintf(stderr, "tgbsctl: ERROR - unable to restore cpuset.cpus for %s: %s\n",
			name, strerror(errno));
	errno = saved_errno;
	return -1;
}

int configure_domain_reclaim(const char *name, int reclaim)
{
	char path[PATH_MAX];
	unsigned long long actual;

	snprintf(path, sizeof(path), "%s/%s/cpu.reclaim", CG_ROOT, name);
	if (write_u64(path, reclaim ? 1 : 0) != 0)
		return -1;
	if (read_u64(path, &actual) != 0)
		return -1;
	if (actual != (unsigned long long)(reclaim ? 1 : 0)) {
		errno = EIO;
		return -1;
	}
	return 0;
}

int configure_domain_limit(const char *name, const char *filename,
		const char *value)
{
	char path[PATH_MAX];
	char normalized[32];
	char actual[32];

	if (strcmp(filename, "memory.max") != 0 &&
	    strcmp(filename, "pids.max") != 0) {
		errno = EINVAL;
		return -1;
	}
	if (parse_cgroup_limit(value, normalized, sizeof(normalized)) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/%s/%s", CG_ROOT, name, filename);
	if (write_text(path, normalized) != 0)
		return -1;
	if (read_text(path, actual, sizeof(actual)) != 0)
		return -1;
	if (strcmp(actual, normalized) != 0) {
		errno = EIO;
		return -1;
	}
	return 0;
}

void error_exit(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "tgbsctl: ERROR - ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

/* cgroup v2 must be mounted as type cgroup2 on /sys/fs/cgroup. */
void verify_cgroup_env(void)
{
	FILE *f;
	char controller[64];
	struct statfs fs;
	int found_cpu = 0;
	int found_cpuset = 0;
	int found_memory = 0;
	int found_pids = 0;

	if (access(CG_ROOT "/cgroup.controllers", R_OK) != 0)
		error_exit(CG_ROOT "/cgroup.controllers is missing, the unified hierarchy is unavailable");

	if (statfs(CG_ROOT, &fs) != 0)
		error_exit("unable to inspect " CG_ROOT ": %s", strerror(errno));
	if ((unsigned long)fs.f_type != CGROUP2_SUPER_MAGIC)
		error_exit(CG_ROOT " is not mounted as cgroup2, TGBS requires cgroup v2");

	f = fopen(CG_ROOT "/cgroup.controllers", "r");
	if (f == NULL)
		error_exit("unable to open " CG_ROOT "/cgroup.controllers");
	while (fscanf(f, "%63s", controller) == 1) {
		if (strcmp(controller, "cpu") == 0)
			found_cpu = 1;
		else if (strcmp(controller, "cpuset") == 0)
			found_cpuset = 1;
		else if (strcmp(controller, "memory") == 0)
			found_memory = 1;
		else if (strcmp(controller, "pids") == 0)
			found_pids = 1;
	}
	fclose(f);

	if (!found_cpu)
		error_exit("the cpu controller is not available in " CG_ROOT "/cgroup.controllers");
	if (!found_cpuset)
		error_exit("the cpuset controller is not available in " CG_ROOT "/cgroup.controllers");
	if (!found_memory)
		error_exit("the memory controller is not available in " CG_ROOT "/cgroup.controllers");
	if (!found_pids)
		error_exit("the pids controller is not available in " CG_ROOT "/cgroup.controllers");
}

/* Terminate every remaining task inside the cgroup. */
int cgroup_kill(const char *name)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/cgroup.kill", CG_ROOT, name);
	if (write_u64(path, 1) == 0)
		return 0;

	/* Either the kernel does not support cgroup.kill, or it returned an
	 * error (e.g. empty cgroup). Fall back to killing the remaining pids. */
	char procs_path[PATH_MAX];
	snprintf(procs_path, sizeof(procs_path), "%s/%s/cgroup.procs", CG_ROOT, name);
	FILE *f = fopen(procs_path, "r");
	if (f == NULL)
		return -1;

	int failed = 0;
	int saved_errno = 0;
	int raw_pid;
	while (fscanf(f, "%d", &raw_pid) == 1) {
		if (raw_pid > 0 && kill((pid_t) raw_pid, SIGKILL) != 0 && errno != ESRCH) {
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

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "run") == 0)
		return cmd_run(argc, argv);
	if (strcmp(argv[1], "list") == 0)
		return cmd_list();
	if (strcmp(argv[1], "inspect") == 0) {
		if (argc < 3) {
			fprintf(stderr, "tgbsctl: ERROR - inspect requires a NAME\n");
			usage(argv[0]);
			return 2;
		}
		return cmd_inspect(argv[2]);
	}
	if (strcmp(argv[1], "kill") == 0) {
		if (argc != 3) {
			fprintf(stderr, "tgbsctl: ERROR - kill requires exactly one NAME\n");
			usage(argv[0]);
			return 2;
		}
		return cmd_kill(argv[2]);
	}
	if (strcmp(argv[1], "pause") == 0 || strcmp(argv[1], "resume") == 0) {
		if (argc != 3) {
			fprintf(stderr, "tgbsctl: ERROR - %s requires exactly one NAME\n", argv[1]);
			usage(argv[0]);
			return 2;
		}
		return cmd_freeze(argv[2], strcmp(argv[1], "pause") == 0);
	}
	if (strcmp(argv[1], "set") == 0) {
		if (argc != 5) {
			fprintf(stderr,
				"tgbsctl: ERROR - set requires NAME, runtime|period|cpus|reclaim, and VALUE\n");
			usage(argv[0]);
			return 2;
		}
		return cmd_set(argv[2], argv[3], argv[4]);
	}
	if (strcmp(argv[1], "channel") == 0)
		return cmd_channel(argc, argv);

	usage(argv[0]);
	return 2;
}
