/*
 * SPDX-License-Identifier: MIT
 *
 * TGBS domain creation, container setup, and lifecycle supervision.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_signal = 0;

static void signal_handler(int sig)
{
	g_signal = sig;
}

static int setup_container_mounts(void)
{
	/* Keep mounts created by the container from propagating to the host. */
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
		return -1;

	/* Hide the inherited host procfs with one tied to this PID namespace. */
	if (mount("proc", "/proc", "proc",
			MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0)
		return -1;
	return 0;
}

static pid_t clone_container_into_cgroup(int cgroup_fd)
{
	struct clone_args args = {
		.flags = CLONE_INTO_CGROUP | CLONE_NEWPID | CLONE_NEWNS |
			CLONE_NEWUTS | CLONE_NEWIPC,
		.exit_signal = SIGCHLD,
		.cgroup = (unsigned long long)cgroup_fd,
	};

	return (pid_t)syscall(SYS_clone3, &args, sizeof(args));
}

/* Bounded cleanup: try rmdir, then kill remaining tasks, then retry. */
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

static void cleanup_run(const char *name)
{
	cleanup_cgroup(name);
	cleanup_metadata(name);
}

/* Write "PID STARTTIME" atomically: temp file then rename. */
static int write_meta(const char *tmp, pid_t pid, unsigned long long starttime)
{
	char buf[64];
	int len;
	int fd;
	ssize_t r;

	len = snprintf(buf, sizeof(buf), "%d %llu", (int)pid,
		(unsigned long long)starttime);
	if (len < 0 || len >= (int)sizeof(buf))
		return -1;

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;

	r = write(fd, buf, (size_t)len);
	close(fd);

	if (r != (ssize_t)len)
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
	int topology_fd = -1;
	int cgroup_fd = -1;

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

	if (strcmp(name, "channels") == 0)
		error_exit("the domain name 'channels' is reserved by the runtime");

	verify_cgroup_env();

	/* Keep the topology stable until the child has entered its cgroup. */
	topology_fd = channel_topology_lock(0);
	if (topology_fd < 0)
		error_exit("unable to lock the channel topology: %s", strerror(errno));
	if (channel_validate_domain(name) != 0) {
		int saved_errno = errno;

		channel_topology_unlock(topology_fd);
		errno = saved_errno;
		error_exit("invalid channel contract for domain %s: %s",
			name, strerror(errno));
	}
	snprintf(name_path, sizeof(name_path), "%s/%s", CG_ROOT, name);
	if (mkdir(name_path, 0755) != 0) {
		if (errno == EEXIST)
			error_exit("TGBS domain %s already exists", name);
		error_exit("unable to create cgroup %s: %s", name_path, strerror(errno));
	}

	snprintf(meta_path, sizeof(meta_path), "%s/%s", RUN_ROOT, name);
	if (mkdir(meta_path, 0755) != 0) {
		int saved_errno = errno;

		cleanup_cgroup(name);
		errno = saved_errno;
		if (errno == EEXIST)
			error_exit("metadata for TGBS domain %s already exists", name);
		error_exit("unable to create metadata for %s: %s", meta_path,
			strerror(errno));
	}
	snprintf(main_pid_path, sizeof(main_pid_path), "%s/main.pid", meta_path);

	/* Configure placement while the group's runtime is still zero, so the
	 * kernel performs bandwidth admission against the final active CPU set. */
	if (cpu_list != NULL && configure_domain_cpus(name, cpu_list) != 0) {
		int saved_errno = errno;
		cleanup_run(name);
		errno = saved_errno;
		error_exit("unable to set cpuset.cpus for %s to '%s': %s",
			name, cpu_list, strerror(errno));
	}

	/* Configure the temporal contract before any task enters the cgroup. */
	char cfg_path[PATH_MAX];
	snprintf(cfg_path, sizeof(cfg_path), "%s/cpu.period_us", name_path);
	if (write_u64(cfg_path, period_us) != 0) {
		int saved_errno = errno;
		cleanup_run(name);
		errno = saved_errno;
		error_exit("unable to set cpu.period_us on %s: %s", cfg_path, strerror(errno));
	}
	snprintf(cfg_path, sizeof(cfg_path), "%s/cpu.runtime_us", name_path);
	if (write_u64(cfg_path, runtime_us) != 0) {
		int saved_errno = errno;
		cleanup_run(name);
		errno = saved_errno;
		error_exit("unable to set cpu.runtime_us on %s: %s", cfg_path, strerror(errno));
	}
	snprintf(cfg_path, sizeof(cfg_path), "%s/cpu.period_us", name_path);
	if (read_u64(cfg_path, &period_us) != 0) {
		int saved_errno = errno;
		cleanup_run(name);
		errno = saved_errno;
		error_exit("unable to verify cpu.period_us on %s", cfg_path);
	}
	snprintf(cfg_path, sizeof(cfg_path), "%s/cpu.runtime_us", name_path);
	if (read_u64(cfg_path, &runtime_us) != 0) {
		int saved_errno = errno;
		cleanup_run(name);
		errno = saved_errno;
		error_exit("unable to verify cpu.runtime_us on %s", cfg_path);
	}

	if (reclaim_set && configure_domain_reclaim(name, reclaim) != 0) {
		int saved_errno = errno;
		cleanup_run(name);
		errno = saved_errno;
		error_exit("unable to set cpu.reclaim for %s: %s", name, strerror(errno));
	}

	/* Signal handling: the cgroup is the lifecycle unit. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	cgroup_fd = open(name_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (cgroup_fd < 0) {
		int saved_errno = errno;

		cleanup_run(name);
		errno = saved_errno;
		error_exit("unable to open cgroup %s: %s", name_path, strerror(errno));
	}

	/* Create PID 1 directly in the configured TGBS cgroup and in its new
	 * namespaces. The supervisor remains in the host namespaces and receives
	 * the child's host PID. */
	pid = clone_container_into_cgroup(cgroup_fd);
	if (pid < 0) {
		int saved_errno = errno;

		close(cgroup_fd);
		cleanup_run(name);
		errno = saved_errno;
		error_exit("clone3 into cgroup %s failed: %s", name_path,
			strerror(errno));
	}

	if (pid == 0) {
		close(cgroup_fd);
		channel_topology_unlock(topology_fd);
		if (setup_container_mounts() != 0) {
			fprintf(stderr,
				"tgbsctl: ERROR - unable to prepare container mounts: %s\n",
				strerror(errno));
			_exit(127);
		}
		execvp(argv[cmd_index], &argv[cmd_index]);
		fprintf(stderr, "tgbsctl: ERROR - failed to execute '%s': %s\n",
			argv[cmd_index], strerror(errno));
		_exit(127);
	}

	close(cgroup_fd);
	channel_topology_unlock(topology_fd);
	topology_fd = -1;

	/* Record the main process identity (pid + /proc starttime) so that
	 * list/inspect can verify it is really the same process after PID reuse. */
	if (read_starttime(pid, &starttime) != 0) {
		cleanup_run(name);
		error_exit("unable to read starttime for %d", pid);
	}
	snprintf(tmp_path, sizeof(tmp_path), "%s/main.pid.tmp", meta_path);
	if (write_meta(tmp_path, pid, starttime) != 0) {
		cleanup_run(name);
		error_exit("unable to write metadata for %s", name);
	}
	if (rename(tmp_path, main_pid_path) != 0) {
		cleanup_run(name);
		error_exit("unable to finalize metadata for %s", name);
	}

	if (waitpid(pid, &status, 0) < 0) {
		int saved_errno = errno;
		cleanup_run(name);
		errno = saved_errno;
		error_exit("waitpid failed: %s", strerror(errno));
	}

	/* A signal arriving during execution tears down the whole cgroup. */
	if (g_signal != 0) {
		int sig = g_signal;
		cgroup_kill(name);
		printf("tgbsctl: interrupted by signal %d, cgroup %s torn down\n", sig, name);
		cleanup_run(name);
		return 128 + sig;
	}

	if (WIFEXITED(status)) {
		int rc = WEXITSTATUS(status);
		printf("tgbsctl: command exited with code %d\n", rc);
		cleanup_run(name);
		return rc;
	}
	if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		printf("tgbsctl: command killed by signal %d\n", sig);
		cleanup_run(name);
		return 128 + sig;
	}

	cleanup_run(name);
	return 1;
}
