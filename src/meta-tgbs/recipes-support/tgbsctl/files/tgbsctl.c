/*
 * SPDX-License-Identifier: MIT
 *
 * tgbsctl - minimal daemonless TGBS runtime and inspection tool
 *
 * The "run" command creates a TGBS cgroup directly under /sys/fs/cgroup,
 * configures its temporal contract, and starts a command inside it. A volatile
 * marker under /run/tgbs records the main process identity so that "list" and
 * "inspect" can correlate userspace processes with their TGBS cgroup.
 *
 * The cgroup lifetime is tied to the main process. When it exits, tgbsctl
 * terminates any remaining tasks, then removes the cgroup and its marker.
 * Observation is read-only and derives the current state from cgroupfs, /proc,
 * and the marker. Control commands act directly on cgroupfs; no daemon or
 * persistent state database is involved.
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
#include <wait.h>
#include <limits.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "tgbsctl.h"

void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s run --name NAME --runtime-us RUNTIME --period-us PERIOD\n"
		"         [--cpus CPU-LIST|inherit] [--reclaim BOOL] COMMAND [ARGS...]\n"
		"  %s list\n"
		"  %s inspect NAME\n"
		"  %s kill NAME\n"
		"  %s pause NAME\n"
		"  %s resume NAME\n"
		"  %s set NAME runtime VALUE_US\n"
		"  %s set NAME period VALUE_US\n"
		"  %s set NAME cpus CPU-LIST|inherit\n"
		"  %s set NAME reclaim 0|1|false|true\n"
		"\n"
		"Commands:\n"
		"  run      Run COMMAND in a TGBS cgroup named NAME under %s.\n"
		"           Run options must follow 'run' and precede COMMAND.\n"
		"           RUNTIME and PERIOD are in microseconds and must satisfy\n"
		"           0 < RUNTIME <= PERIOD.\n"
		"           CPU-LIST uses the cpuset list syntax, for example 0-1 or 0,2;\n"
		"           inherit selects the cgroup root's effective CPU list.\n"
		"           BOOL accepts 0, 1, false, or true.\n"
		"  list     List the TGBS domains known to this runtime.\n"
		"  inspect  Show the state, temporal contract, and processes for NAME.\n"
		"  kill     Kill every process in NAME. The run supervisor then cleans up.\n"
		"  pause    Freeze every process in NAME.\n"
		"  resume   Unfreeze every process in NAME.\n"
		"  set      Change one value of NAME's temporal contract.\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, CG_ROOT);
}

int is_valid_name(const char *name)
{
	size_t i;

	if (name == NULL || name[0] == '\0' || strlen(name) > 255)
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
	char source[64], target[64], type[64];
	int found_cgroup2 = 0;
	int found_cpu = 0;

	if (access(CG_ROOT "/cgroup.controllers", R_OK) != 0)
		error_exit(CG_ROOT "/cgroup.controllers is missing, the unified hierarchy is unavailable");

	f = fopen("/proc/mounts", "r");
	if (f == NULL)
		error_exit("/proc/mounts is unreadable, the boot environment is incomplete");

	while (fscanf(f, "%63s %63s %63s", source, target, type) == 3) {
		if (strcmp(target, CG_ROOT) == 0 && strcmp(type, "cgroup2") == 0) {
			found_cgroup2 = 1;
			break;
		}
	}
	fclose(f);

	if (!found_cgroup2)
		error_exit(CG_ROOT " is not mounted as cgroup2, TGBS requires cgroup v2");

	f = fopen(CG_ROOT "/cgroup.controllers", "r");
	if (f == NULL)
		error_exit("unable to open " CG_ROOT "/cgroup.controllers");
	while (fscanf(f, "%63s", type) == 1) {
		if (strcmp(type, "cpu") == 0) {
			found_cpu = 1;
			break;
		}
	}
	fclose(f);

	if (!found_cpu)
		error_exit("the cpu controller is not available in " CG_ROOT "/cgroup.controllers");
}

static volatile sig_atomic_t g_signal = 0;

static void signal_handler(int sig)
{
	g_signal = sig;
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

/* Borned cleanup: try rmdir, then kill remaining tasks, then retry. */
static void cleanup_cgroup(const char *name)
{
	char path[PATH_MAX];
	int waited = 0;

	while (waited < 10) {
		snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
		if (rmdir(path) == 0) {
			printf("tgbsctl: removed cgroup %s\n", name);
			return;
		}
		if (errno == EBUSY) {
			cgroup_kill(name);
			waited++;
			usleep(10000);
			continue;
		}
		break;
	}
	fprintf(stderr, "tgbsctl: ERROR - unable to remove cgroup %s: %s\n",
		name, strerror(errno));
}

/* Remove the runtime ownership marker once the cgroup is gone. Also remove a
 * temporary marker left by a failed write/rename before removing the directory. */
static void cleanup_metadata(const char *name)
{
	char meta_path[PATH_MAX];
	char main_pid_path[PATH_MAX + 32];
	char tmp_path[PATH_MAX + 32];

	snprintf(meta_path, sizeof(meta_path), "%s/%s", RUN_ROOT, name);
	snprintf(main_pid_path, sizeof(main_pid_path), "%s/main.pid", meta_path);
	snprintf(tmp_path, sizeof(tmp_path), "%s/main.pid.tmp", meta_path);
	unlink(main_pid_path);
	unlink(tmp_path);
	if (rmdir(meta_path) != 0 && errno != ENOENT)
		fprintf(stderr, "tgbsctl: ERROR - unable to remove metadata for %s: %s\n",
			name, strerror(errno));
}

/* Write "PID STARTTIME" atomically: temp file then rename. */
static int write_meta(const char *tmp, pid_t pid, unsigned long long starttime)
{
	char buf[64];
	int len;
	int fd;
	ssize_t r;

	len = snprintf(buf, sizeof(buf), "%d %llu", (int) pid,
		(unsigned long long) starttime);
	if (len < 0 || len >= (int) sizeof(buf))
		return -1;

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;

	r = write(fd, buf, (size_t) len);
	close(fd);

	if (r != (ssize_t) len)
		return -1;
	return 0;
}

int cmd_run(int argc, char **argv)
{
	const char *name = NULL;
	const char *cpu_list = NULL;
	unsigned long long runtime_us = 0;
	unsigned long long period_us = 0;
	int reclaim = 0;
	int reclaim_set = 0;
	int opt;
	int cmd_index = 0;
	char name_path[288];
	char meta_path[288];
	char main_pid_path[PATH_MAX];
	char tmp_path[PATH_MAX];
	pid_t pid;
	unsigned long long starttime = 0;
	int status;
	struct sigaction sa;

	/* The 'run' subcommand is argv[1]; options follow it. Start getopt at
	 * argv[2] so 'run' is skipped; the command is whatever getopt leaves at
	 * optind once it stops at the first non-option after the options. */
	if (argc < 2 || strcmp(argv[1], "run") != 0) {
		fprintf(stderr, "tgbsctl: ERROR - no 'run' subcommand given\n");
		usage(argv[0]);
		return 2;
	}

	static const struct option longopts[] = {
		{"name", required_argument, NULL, 'n'},
		{"runtime-us", required_argument, NULL, 'r'},
		{"period-us", required_argument, NULL, 'p'},
		{"cpus", required_argument, NULL, 'c'},
		{"reclaim", required_argument, NULL, 'R'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	optind = 2;
	while ((opt = getopt_long(argc, argv, "+hc:n:r:p:R:", longopts, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage(argv[0]);
			return 0;
		case 'n':
			name = optarg;
			break;
		case 'r':
			if (parse_positive(optarg, &runtime_us) != 0)
				error_exit("--runtime-us must be a strictly positive integer");
			break;
		case 'p':
			if (parse_positive(optarg, &period_us) != 0)
				error_exit("--period-us must be a strictly positive integer");
			break;
		case 'c':
			cpu_list = optarg;
			break;
		case 'R':
			if (parse_bool(optarg, &reclaim) != 0)
				error_exit("--reclaim must be one of 0, 1, false, or true");
			reclaim_set = 1;
			break;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	cmd_index = optind;
	if (cmd_index >= argc) {
		fprintf(stderr, "tgbsctl: ERROR - no COMMAND given\n");
		usage(argv[0]);
		return 2;
	}

	if (name == NULL || !is_valid_name(name)) {
		fprintf(stderr, "tgbsctl: ERROR - --name must match [a-zA-Z0-9_.-]+ (max 255, no '/'); got '%s'\n",
			name == NULL ? "(null)" : name);
		return 1;
	}
	if (runtime_us == 0 || period_us == 0) {
		fprintf(stderr, "tgbsctl: ERROR - --runtime-us and --period-us must both be set\n");
		usage(argv[0]);
		return 2;
	}
	if (runtime_us > period_us)
		error_exit("--runtime-us must be <= --period-us");

	verify_cgroup_env();

	snprintf(name_path, sizeof(name_path), "%s/%s", CG_ROOT, name);
	if (mkdir(name_path, 0755) != 0 && errno != EEXIST)
		error_exit("unable to create cgroup %s: %s", name_path, strerror(errno));

	snprintf(meta_path, sizeof(meta_path), "%s/%s", RUN_ROOT, name);
	if (mkdir(meta_path, 0755) != 0 && errno != EEXIST)
		error_exit("unable to create metadata for %s: %s", name_path, strerror(errno));
	snprintf(main_pid_path, sizeof(main_pid_path), "%s/main.pid", meta_path);

	/* Configure placement while the group's runtime is still zero, so the
	 * kernel performs bandwidth admission against the final active CPU set. */
	if (cpu_list != NULL && configure_domain_cpus(name, cpu_list) != 0) {
		int saved_errno = errno;
		cleanup_cgroup(name);
		cleanup_metadata(name);
		errno = saved_errno;
		error_exit("unable to set cpuset.cpus for %s to '%s': %s",
			name, cpu_list, strerror(errno));
	}

	/* Configure the temporal contract before any task enters the cgroup. */
	char cfg_path[PATH_MAX];
	snprintf(cfg_path, sizeof(cfg_path), "%s/cpu.period_us", name_path);
	if (write_u64(cfg_path, period_us) != 0)
		error_exit("unable to set cpu.period_us on %s: %s", cfg_path, strerror(errno));
	snprintf(cfg_path, sizeof(cfg_path), "%s/cpu.runtime_us", name_path);
	if (write_u64(cfg_path, runtime_us) != 0)
		error_exit("unable to set cpu.runtime_us on %s: %s", cfg_path, strerror(errno));
	snprintf(cfg_path, sizeof(cfg_path), "%s/cpu.period_us", name_path);
	if (read_u64(cfg_path, &period_us) != 0)
		error_exit("unable to verify cpu.period_us on %s", cfg_path);
	snprintf(cfg_path, sizeof(cfg_path), "%s/cpu.runtime_us", name_path);
	if (read_u64(cfg_path, &runtime_us) != 0)
		error_exit("unable to verify cpu.runtime_us on %s", cfg_path);

	if (reclaim_set && configure_domain_reclaim(name, reclaim) != 0) {
		int saved_errno = errno;
		cleanup_cgroup(name);
		cleanup_metadata(name);
		errno = saved_errno;
		error_exit("unable to set cpu.reclaim for %s: %s", name, strerror(errno));
	}

	/* Signal handling: the cgroup is the lifecycle unit. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	pid = fork();
	if (pid < 0)
		error_exit("fork failed: %s", strerror(errno));

	if (pid == 0) {
		/* Child: pause immediately so the parent can move us before we run. */
		raise(SIGSTOP);
		execvp(argv[cmd_index], &argv[cmd_index]);
		fprintf(stderr, "tgbsctl: ERROR - failed to execute '%s': %s\n",
			argv[cmd_index], strerror(errno));
		_exit(127);
	}

	/* Parent: wait for the stop, then move the child into the cgroup. */
	if (waitpid(pid, &status, WUNTRACED) < 0)
		error_exit("waitpid failed: %s", strerror(errno));
	if (!WIFSTOPPED(status))
		error_exit("child did not pause as expected");

	snprintf(cfg_path, sizeof(cfg_path), "%s/cgroup.procs", name_path);
	if (write_u64(cfg_path, (unsigned long long) pid) != 0) {
		/* Child is stuck paused; kill it before bailing out. */
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		cleanup_cgroup(name);
		error_exit("unable to move child into %s: %s", cfg_path, strerror(errno));
	}

	/* Resume: the child now execs inside the TGBS cgroup. */
	kill(pid, SIGCONT);

	/* Record the main process identity (pid + /proc starttime) so that
	 * list/inspect can verify it is really the same process after PID reuse. */
	if (read_starttime(pid, &starttime) != 0) {
		cleanup_cgroup(name);
		cleanup_metadata(name);
		error_exit("unable to read starttime for %d", pid);
	}
	snprintf(tmp_path, sizeof(tmp_path), "%s/main.pid.tmp", meta_path);
	if (write_meta(tmp_path, pid, starttime) != 0) {
		cleanup_cgroup(name);
		cleanup_metadata(name);
		error_exit("unable to write metadata for %s", name);
	}
	if (rename(tmp_path, main_pid_path) != 0) {
		cleanup_cgroup(name);
		cleanup_metadata(name);
		error_exit("unable to finalize metadata for %s", name);
	}

	if (waitpid(pid, &status, 0) < 0) {
		cleanup_cgroup(name);
		error_exit("waitpid failed: %s", strerror(errno));
	}

	/* A signal arriving during execution tears down the whole cgroup. */
	if (g_signal != 0) {
		int sig = g_signal;
		cgroup_kill(name);
		printf("tgbsctl: interrupted by signal %d, cgroup %s torn down\n", sig, name);
		cleanup_cgroup(name);
		return 128 + sig;
	}

	if (WIFEXITED(status)) {
		int rc = WEXITSTATUS(status);
		printf("tgbsctl: command exited with code %d\n", rc);
		cleanup_cgroup(name);
		cleanup_metadata(name);
		return rc;
	}
	if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		printf("tgbsctl: command killed by signal %d\n", sig);
		cleanup_cgroup(name);
		cleanup_metadata(name);
		return 128 + sig;
	}

	cleanup_cgroup(name);
	cleanup_metadata(name);
	return 1;
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

	usage(argv[0]);
	return 2;
}
