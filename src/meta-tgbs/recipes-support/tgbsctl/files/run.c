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
#include <linux/capability.h>
#include <linux/mount.h>
#include <linux/sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_signal = 0;

enum run_option {
	OPT_MEMORY_MAX = 256,
	OPT_PIDS_MAX
};

static void signal_handler(int sig)
{
	g_signal = sig;
}

static int drop_cap_sys_admin(void)
{
	struct __user_cap_header_struct header = {
		.version = _LINUX_CAPABILITY_VERSION_3,
		.pid = 0,
	};
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
	const unsigned int index = CAP_SYS_ADMIN / 32U;
	const unsigned int mask = 1U << (CAP_SYS_ADMIN % 32U);
	int bounded;

	/* Dropping from the bounding set is irreversible for this process tree and
	 * prevents UID 0 exec semantics from granting CAP_SYS_ADMIN again. */
	if (prctl(PR_CAPBSET_DROP, CAP_SYS_ADMIN, 0, 0, 0) != 0)
		return -1;
	if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER,
			CAP_SYS_ADMIN, 0, 0) != 0)
		return -1;

	memset(data, 0, sizeof(data));
	if (syscall(SYS_capget, &header, data) != 0)
		return -1;
	data[index].effective &= ~mask;
	data[index].permitted &= ~mask;
	data[index].inheritable &= ~mask;
	if (syscall(SYS_capset, &header, data) != 0)
		return -1;

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
		return -1;
	bounded = prctl(PR_CAPBSET_READ, CAP_SYS_ADMIN, 0, 0, 0);
	if (bounded < 0)
		return -1;
	if (bounded != 0) {
		errno = EIO;
		return -1;
	}

	memset(data, 0, sizeof(data));
	if (syscall(SYS_capget, &header, data) != 0)
		return -1;
	if ((data[index].effective & mask) != 0 ||
	    (data[index].permitted & mask) != 0 ||
	    (data[index].inheritable & mask) != 0) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int mkdir_if_missing(const char *path, mode_t mode)
{
	if (mkdir(path, mode) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static int move_mount_to_path(int fd, const char *target)
{
	return (int)syscall(SYS_move_mount, fd, "", AT_FDCWD, target,
		MOVE_MOUNT_F_EMPTY_PATH);
}

static int remount_bind(const char *path, int read_only)
{
	unsigned long flags = MS_REMOUNT | MS_BIND | MS_NOSUID | MS_NODEV |
		MS_NOEXEC;

	if (read_only)
		flags |= MS_RDONLY;
	return mount(NULL, path, NULL, flags, NULL);
}

static int report_mount_error(const char *operation, const char *path)
{
	int saved_errno = errno;

	fprintf(stderr, "tgbsctl: ERROR - %s %s: %s\n",
		operation, path, strerror(saved_errno));
	errno = saved_errno;
	return -1;
}

static int setup_overlay_root(void)
{
	static const char stage[] = "/run/.tgbs-root";
	static const char upper[] = "/run/.tgbs-root/upper";
	static const char work[] = "/run/.tgbs-root/work";
	static const char merged[] = "/run/.tgbs-root/merged";
	static const char old_root[] = "/run/.tgbs-root/merged/.tgbs-old-root";
	static const char overlay_options[] =
		"lowerdir=/,upperdir=/run/.tgbs-root/upper,"
		"workdir=/run/.tgbs-root/work";

	/* The staging tmpfs contains the private writable layer. It is mounted on
	 * /run so creating its directories never modifies the host rootfs. */
	if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV,
			"mode=0700") != 0)
		return report_mount_error("unable to mount rootfs staging tmpfs at",
			"/run");
	if (mkdir_if_missing(stage, 0700) != 0 ||
	    mkdir_if_missing(upper, 0700) != 0 ||
	    mkdir_if_missing(work, 0700) != 0 ||
	    mkdir_if_missing(merged, 0700) != 0)
		return report_mount_error("unable to create rootfs staging directory at",
			stage);

	if (mount("overlay", merged, "overlay", 0, overlay_options) != 0)
		return report_mount_error("unable to mount overlay root at", merged);
	if (mkdir(old_root, 0700) != 0)
		return report_mount_error("unable to create old-root mountpoint at",
			old_root);
	if (syscall(SYS_pivot_root, merged, old_root) != 0)
		return report_mount_error("unable to pivot to overlay root at", merged);
	if (chdir("/") != 0)
		return report_mount_error("unable to enter overlay root at", "/");

	/* Preserve the current device view for prototype compatibility. Since the
	 * entire mount namespace is private, this bind cannot propagate back. */
	if (mount("/.tgbs-old-root/dev", "/dev", NULL,
			MS_BIND | MS_REC, NULL) != 0)
		return report_mount_error("unable to expose host device view at", "/dev");

	if (umount2("/.tgbs-old-root", MNT_DETACH) != 0)
		return report_mount_error("unable to detach old root at",
			"/.tgbs-old-root");
	if (rmdir("/.tgbs-old-root") != 0)
		return report_mount_error("unable to remove old-root mountpoint at",
			"/.tgbs-old-root");
	return 0;
}

static int setup_channel_mounts(const struct channel_mount_plan *plan)
{
	size_t i;

	if (mkdir_if_missing(RUN_ROOT, 0755) != 0 ||
	    mkdir_if_missing(CHANNEL_ROOT, 0755) != 0)
		return -1;

	/* Create every mountpoint before making the channel index immutable. */
	for (i = 0; i < plan->count; i++) {
		char target[PATH_MAX];

		if (snprintf(target, sizeof(target), "%s/%s", CHANNEL_ROOT,
				plan->entries[i].name) >= (int)sizeof(target)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		if (mkdir(target, 0755) != 0)
			return -1;
	}

	/* Prevent applications from adding channel names to the private index. */
	if (mount(CHANNEL_ROOT, CHANNEL_ROOT, NULL, MS_BIND, NULL) != 0)
		return report_mount_error("unable to bind channel index at", CHANNEL_ROOT);
	if (remount_bind(CHANNEL_ROOT, 1) != 0)
		return report_mount_error("unable to make channel index read-only at",
			CHANNEL_ROOT);

	for (i = 0; i < plan->count; i++) {
		const struct channel_mount_entry *entry = &plan->entries[i];
		char target[PATH_MAX];
		char endpoint[PATH_MAX];
		char source_lock[PATH_MAX];

		if (snprintf(target, sizeof(target), "%s/%s", CHANNEL_ROOT,
				entry->name) >= (int)sizeof(target) ||
		    snprintf(endpoint, sizeof(endpoint), "%s/endpoint", target) >=
				(int)sizeof(endpoint) ||
		    snprintf(source_lock, sizeof(source_lock), "%s/source.lock",
				endpoint) >= (int)sizeof(source_lock)) {
			errno = ENAMETOOLONG;
			return -1;
		}

		/* Contracts and lifetime locks are always exposed read-only. */
		if (move_mount_to_path(entry->channel_fd, target) != 0)
			return report_mount_error("unable to expose channel at", target);
		if (remount_bind(target, 1) != 0)
			return report_mount_error("unable to make channel read-only at",
				target);

		if ((entry->type == TGBS_CONTRACT_QUEUING &&
		     (entry->roles & CHANNEL_MOUNT_DESTINATION)) ||
		    (entry->type == TGBS_CONTRACT_SAMPLING &&
		     (entry->roles & CHANNEL_MOUNT_SOURCE))) {
			/* Queuing destinations create the socket; sampling sources will
			 * publish the latest value in the writable endpoint directory. */
			if (move_mount_to_path(entry->endpoint_fd, endpoint) != 0)
				return report_mount_error("unable to expose writable endpoint at",
					endpoint);
			if (remount_bind(endpoint, 0) != 0)
				return report_mount_error("unable to make endpoint writable at",
					endpoint);
		}

		/* Every participant sees source.lock through an explicit file mount.
		 * It is writable only when the domain also owns the source role. */
		if (move_mount_to_path(entry->source_lock_fd, source_lock) != 0)
			return report_mount_error("unable to expose source lock at",
				source_lock);
		if (remount_bind(source_lock,
				!(entry->roles & CHANNEL_MOUNT_SOURCE)) != 0)
			return report_mount_error(
				(entry->roles & CHANNEL_MOUNT_SOURCE) ?
				"unable to make source lock writable at" :
				"unable to make source lock read-only at",
				source_lock);
	}
	return 0;
}

static int setup_container_mounts(const struct channel_mount_plan *plan)
{
	/* Keep mounts created by the container from propagating to the host. */
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
		return -1;
	if (setup_overlay_root() != 0)
		return -1;

	/* Yocto keeps /tmp and /var/log below a volatile tmpfs mounted by init.
	 * Submount contents are intentionally absent from an overlay lower layer,
	 * so recreate the corresponding private directories in the upper layer. */
	if (mkdir_if_missing("/var/volatile", 0755) != 0 ||
	    mkdir_if_missing("/var/volatile/tmp", 01777) != 0 ||
	    mkdir_if_missing("/var/volatile/log", 0755) != 0)
		return report_mount_error("unable to prepare private volatile path at",
			"/var/volatile");
	if (mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV,
			"mode=1777") != 0)
		return report_mount_error("unable to mount private tmpfs at", "/tmp");
	if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV,
			"mode=0755") != 0)
		return report_mount_error("unable to mount private tmpfs at", "/run");
	if (mkdir_if_missing("/run/lock", 0755) != 0)
		return report_mount_error("unable to create private runtime path at",
			"/run/lock");
	if (setup_channel_mounts(plan) != 0)
		return -1;

	/* Expose host device information without allowing sysfs configuration. */
	if (mount("sysfs", "/sys", "sysfs",
			MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0)
		return report_mount_error("unable to mount read-only sysfs at", "/sys");

	/* In the new cgroup namespace this fresh mount is rooted at the domain. */
	if (mount("cgroup2", CG_ROOT, "cgroup2",
			MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0)
		return report_mount_error("unable to mount private cgroup view at",
			CG_ROOT);

	/* Hide the inherited host procfs with one tied to this PID namespace. */
	if (mount("proc", "/proc", "proc",
			MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0)
		return report_mount_error("unable to mount private procfs at", "/proc");
	return 0;
}

static pid_t clone_container_into_cgroup(int cgroup_fd)
{
	struct clone_args args = {
		.flags = CLONE_INTO_CGROUP | CLONE_NEWPID | CLONE_NEWNS |
			CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWCGROUP,
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
	const char *hostname = NULL;
	const char *cpu_list = NULL;
	unsigned long long runtime_us = 0;
	unsigned long long period_us = 0;
	char memory_max[32] = "max";
	char pids_max[32] = "max";
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
	struct channel_mount_plan channel_mounts = {0};

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
		{"hostname", required_argument, NULL, 'H'},
		{"runtime-us", required_argument, NULL, 'r'},
		{"period-us", required_argument, NULL, 'p'},
		{"cpus", required_argument, NULL, 'c'},
		{"reclaim", required_argument, NULL, 'R'},
		{"memory-max", required_argument, NULL, OPT_MEMORY_MAX},
		{"pids-max", required_argument, NULL, OPT_PIDS_MAX},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	optind = 2;
	while ((opt = getopt_long(argc, argv, "+hH:c:n:r:p:R:", longopts, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage(argv[0]);
			return 0;
		case 'n':
			name = optarg;
			break;
		case 'H':
			hostname = optarg;
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
		case OPT_MEMORY_MAX:
			if (parse_cgroup_limit(optarg, memory_max,
					sizeof(memory_max)) != 0)
				error_exit("--memory-max must be a positive byte count or max");
			break;
		case OPT_PIDS_MAX:
			if (parse_cgroup_limit(optarg, pids_max,
					sizeof(pids_max)) != 0)
				error_exit("--pids-max must be a positive count or max");
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
	if (hostname != NULL &&
	    (!is_valid_name(hostname) || strlen(hostname) > HOST_NAME_MAX)) {
		fprintf(stderr,
			"tgbsctl: ERROR - --hostname must match [a-zA-Z0-9_.-]+ "
			"and contain at most %d characters; got '%s'\n",
			HOST_NAME_MAX, hostname);
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
	if (channel_mount_plan_prepare(name, &channel_mounts) != 0) {
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

	/* Resource limits are configured before the first task enters the group. */
	if (configure_domain_limit(name, "memory.max", memory_max) != 0) {
		int saved_errno = errno;

		cleanup_run(name);
		errno = saved_errno;
		error_exit("unable to set memory.max for %s to '%s': %s",
			name, memory_max, strerror(errno));
	}
	if (configure_domain_limit(name, "pids.max", pids_max) != 0) {
		int saved_errno = errno;

		cleanup_run(name);
		errno = saved_errno;
		error_exit("unable to set pids.max for %s to '%s': %s",
			name, pids_max, strerror(errno));
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
		channel_mount_plan_close(&channel_mounts);
		cleanup_run(name);
		errno = saved_errno;
		error_exit("clone3 into cgroup %s failed: %s", name_path,
			strerror(errno));
	}

	if (pid == 0) {
		int setup_errno;

		close(cgroup_fd);
		if (setup_container_mounts(&channel_mounts) != 0) {
			setup_errno = errno;
			channel_mount_plan_close(&channel_mounts);
			channel_topology_unlock(topology_fd);
			fprintf(stderr,
				"tgbsctl: ERROR - unable to prepare container mounts: %s\n",
				strerror(setup_errno));
			_exit(127);
		}
		channel_mount_plan_close(&channel_mounts);
		channel_topology_unlock(topology_fd);
		if (hostname != NULL && sethostname(hostname, strlen(hostname)) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to set hostname to '%s': %s\n",
				hostname, strerror(errno));
			_exit(127);
		}
		if (drop_cap_sys_admin() != 0) {
			fprintf(stderr,
				"tgbsctl: ERROR - unable to drop CAP_SYS_ADMIN: %s\n",
				strerror(errno));
			_exit(127);
		}
		execvp(argv[cmd_index], &argv[cmd_index]);
		fprintf(stderr, "tgbsctl: ERROR - failed to execute '%s': %s\n",
			argv[cmd_index], strerror(errno));
		_exit(127);
	}

	close(cgroup_fd);
	channel_mount_plan_close(&channel_mounts);
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
