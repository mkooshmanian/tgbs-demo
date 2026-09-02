/*
 * SPDX-License-Identifier: MIT
 *
 * tgbsctl - minimal daemonless TGBS runtime launcher
 *
 * Run a command inside a TGBS cgroup created directly under /sys/fs/cgroup.
 *
 * The cgroup lifetime is tied to the main process: when it exits, any remaining
 * tasks inside the cgroup are terminated before the cgroup is removed.
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

#define CG_ROOT "/sys/fs/cgroup"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s run --name NAME --runtime-us RUNTIME --period-us PERIOD COMMAND [ARGS...]\n"
		"\n"
		"Options must precede the subcommand 'run'.\n"
		"Run COMMAND inside a TGBS cgroup named NAME created under %s.\n"
		"Runtime and period are in microseconds with 0 < runtime <= period.\n",
		prog, CG_ROOT);
}

static int is_valid_name(const char *name)
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
static int parse_positive(const char *text, unsigned long long *out)
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

static int write_u64(const char *path, unsigned long long value)
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

/* Read back a cgroup file and confirm the value matches, so a silent kernel
 * write cannot leave a cgroup configured with a stale value. */
static int read_u64(const char *path, unsigned long long *out)
{
	char buf[32];
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
	unsigned long long value = strtoull(buf, NULL, 10);
	if (errno != 0 || value == 0)
		return -1;
	*out = value;
	return 0;
}

static void error_exit(const char *fmt, ...)
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
static void verify_cgroup_env(void)
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
static void cgroup_kill(const char *name)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/cgroup.kill", CG_ROOT, name);
	if (write_u64(path, 1) == 0)
		return;

	/* Either the kernel does not support cgroup.kill, or it returned an
	 * error (e.g. empty cgroup). Fall back to killing the remaining pids. */
	int fd;
	char procs_path[PATH_MAX];
	snprintf(procs_path, sizeof(procs_path), "%s/%s/cgroup.procs", CG_ROOT, name);
	fd = open(procs_path, O_RDWR);
	if (fd < 0)
		return;
	while (1) {
		char pidbuf[32];
		ssize_t r = read(fd, pidbuf, sizeof(pidbuf) - 1);
		if (r <= 0)
			break;
		pidbuf[r] = '\0';
		for (char *p = pidbuf; *p != '\0'; p++) {
			if (isdigit((unsigned char) *p)) {
				pid_t pid = (pid_t) strtoul(p, &p, 10);
				if (pid > 0)
					kill(pid, SIGKILL);
			}
		}
	}
	close(fd);
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

int main(int argc, char **argv)
{
	const char *name = NULL;
	unsigned long long runtime_us = 0;
	unsigned long long period_us = 0;
	int opt;
	int cmd_index = 0;
	char name_path[288];
	pid_t pid;
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
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	optind = 2;
	while ((opt = getopt_long(argc, argv, "+hn:r:p:", longopts, NULL)) != -1) {
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
		return rc;
	}
	if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		printf("tgbsctl: command killed by signal %d\n", sig);
		cleanup_cgroup(name);
		return 128 + sig;
	}

	cleanup_cgroup(name);
	return 1;
}
