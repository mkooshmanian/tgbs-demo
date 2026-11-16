/*
 * SPDX-License-Identifier: MIT
 *
 * TGBS domain creation, container setup, and lifecycle supervision.
 */

#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <linux/close_range.h>
#include <poll.h>
#include <fcntl.h>
#include <stdlib.h>
#include <linux/capability.h>
#include <linux/mount.h>
#include <linux/sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Called only in the newly created child. The error pipe is CLOEXEC: success
 * closes it atomically at exec; failures transmit errno before _exit. */
void child_fail(int fd, int error)
{
	ssize_t n;

	if (!error) {
		error = EIO;
	}

	do {
		n = write(fd, &error, sizeof(error));
	} while (n < 0 && errno == EINTR);
	_exit(127);
}

void child_prepare(void)
{
	struct sigaction action = { .sa_handler = SIG_DFL };
	sigset_t empty;

	sigemptyset(&action.sa_mask);
	for (int sig = 1; sig < NSIG; sig++) {
		if (sig != SIGKILL && sig != SIGSTOP) {
			sigaction(sig, &action, NULL);
		}
	}

	sigemptyset(&empty);
	sigprocmask(SIG_SETMASK, &empty, NULL);

	/* Prevent application-owned descriptors leaking into the container while
	 * retaining the runtime's descriptors until setup has consumed them. */
	syscall(SYS_close_range, 3U, ~0U, CLOSE_RANGE_CLOEXEC);
}

int wait_child(pid_t pid, int cancel_fd, const char *domain, int error_fd, int cancel_signal,
	tgbs_exit_status_t *status)
{
	int pid_fd = (int)syscall(SYS_pidfd_open, pid, 0);
	int failure = 0, child_status = 0, setup_error = 0, setup_done = 0;
	struct pollfd fds[3] = {
		{ .fd = pid_fd, .events = POLLIN },
		{ .fd = cancel_fd, .events = POLLIN },
		{ .fd = error_fd, .events = POLLIN },
	};

	if (pid_fd < 0) {
		failure = errno;
	}
	while (!failure) {
		int n;

		do {
			n = poll(fds, 3, -1);
		} while (n < 0 && errno == EINTR);
		if (n < 0) {
			failure = errno;
			break;
		}

		if (fds[1].revents) {
			if (fds[1].revents & POLLNVAL) {
				failure = EBADF;
				break;
			}

			status->cancelled = 1;
			if (domain && cgroup_kill(domain) != 0) {
				failure = errno;
				break;
			}

			if (kill(pid, cancel_signal) != 0 && errno != ESRCH) {
				failure = errno;
				break;
			}

			fds[1].fd = -1;
		}

		if (fds[2].revents) {
			ssize_t length;

			do {
				length = read(error_fd, &setup_error, sizeof(setup_error));
			} while (length < 0 && errno == EINTR);
			if (length < 0) {
				failure = errno;
				break;
			}

			if (length != 0 && length != sizeof(setup_error)) {
				failure = EPROTO;
				break;
			}

			setup_done = 1;
			fds[2].fd = -1;
		}

		if (fds[0].revents) {
			break;
		}
	}

	if (failure) {
		if (domain) {
			cgroup_kill(domain);
		}

		kill(pid, SIGKILL);
	}

	/* The child may exit before poll has consumed its setup status. */
	if (!setup_done) {
		ssize_t n;

		do {
			n = read(error_fd, &setup_error, sizeof(setup_error));
		} while (n < 0 && errno == EINTR);
		if (n < 0 && !failure) {
			failure = errno;
		}

		if (n > 0 && n != sizeof(setup_error) && !failure) {
			failure = EPROTO;
		}
	}

	close(error_fd);
	if (pid_fd >= 0) {
		close(pid_fd);
	}

	pid_t reaped;

	do {
		reaped = waitpid(pid, &child_status, 0);
	} while (reaped < 0 && errno == EINTR);
	if (reaped < 0) {
		if (!failure) {
			failure = errno;
		}
	} else if (WIFEXITED(child_status)) {
		status->exited = 1;
		status->exit_code = WEXITSTATUS(child_status);
	} else if (WIFSIGNALED(child_status)) {
		status->term_signal = WTERMSIG(child_status);
	}

	if (setup_error && !failure) {
		failure = setup_error;
	}

	if (failure) {
		errno = failure;
		return -1;
	}

	return 0;
}

int drop_cap_sys_admin(void)
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
	if (prctl(PR_CAPBSET_DROP, CAP_SYS_ADMIN, 0, 0, 0) != 0) {
		return -1;
	}

	if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER, CAP_SYS_ADMIN, 0, 0) != 0) {
		return -1;
	}

	memset(data, 0, sizeof(data));
	if (syscall(SYS_capget, &header, data) != 0) {
		return -1;
	}

	data[index].effective &= ~mask;
	data[index].permitted &= ~mask;
	data[index].inheritable &= ~mask;
	if (syscall(SYS_capset, &header, data) != 0) {
		return -1;
	}

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
		return -1;
	}

	bounded = prctl(PR_CAPBSET_READ, CAP_SYS_ADMIN, 0, 0, 0);
	if (bounded < 0) {
		return -1;
	}

	if (bounded != 0) {
		errno = EIO;
		return -1;
	}

	memset(data, 0, sizeof(data));
	if (syscall(SYS_capget, &header, data) != 0) {
		return -1;
	}

	if ((data[index].effective & mask) != 0 || (data[index].permitted & mask) != 0 ||
		(data[index].inheritable & mask) != 0) {
		errno = EIO;
		return -1;
	}

	return 0;
}

static int mkdir_if_missing(const char *path, mode_t mode)
{
	if (mkdir(path, mode) == 0 || errno == EEXIST) {
		return 0;
	}

	return -1;
}

static int move_mount_to_path(int fd, const char *target)
{
	return (int)syscall(SYS_move_mount, fd, "", AT_FDCWD, target, MOVE_MOUNT_F_EMPTY_PATH);
}

static int remount_bind(const char *path, int read_only)
{
	unsigned long flags = MS_REMOUNT | MS_BIND | MS_NOSUID | MS_NODEV | MS_NOEXEC;

	if (read_only) {
		flags |= MS_RDONLY;
	}

	return mount(NULL, path, NULL, flags, NULL);
}

static int report_mount_error(const char *operation, const char *path)
{
	int saved_errno = errno;

	(void)operation;
	(void)path;
	errno = saved_errno;
	return -1;
}

static int setup_channel_mounts(const struct channel_mount_plan *plan)
{
	size_t i;

	if (mkdir_if_missing(RUN_ROOT, 0755) != 0 || mkdir_if_missing(CHANNEL_ROOT, 0755) != 0) {
		return -1;
	}

	/* Create every mountpoint before making the channel index immutable. */
	for (i = 0; i < plan->count; i++) {
		char target[PATH_MAX];

		if (snprintf(target, sizeof(target), "%s/%s", CHANNEL_ROOT,
			    plan->entries[i].name) >= (int)sizeof(target)) {
			errno = ENAMETOOLONG;
			return -1;
		}

		if (mkdir(target, 0755) != 0) {
			return -1;
		}
	}

	/* Prevent applications from adding channel names to the private index. */
	if (mount(CHANNEL_ROOT, CHANNEL_ROOT, NULL, MS_BIND, NULL) != 0) {
		return report_mount_error("unable to bind channel index at", CHANNEL_ROOT);
	}

	if (remount_bind(CHANNEL_ROOT, 1) != 0) {
		return report_mount_error(
			"unable to make channel index read-only at", CHANNEL_ROOT);
	}

	for (i = 0; i < plan->count; i++) {
		const struct channel_mount_entry *entry = &plan->entries[i];
		char target[PATH_MAX];
		char endpoint[PATH_MAX];
		char source_lock[PATH_MAX];

		if (snprintf(target, sizeof(target), "%s/%s", CHANNEL_ROOT, entry->name) >=
				(int)sizeof(target) ||
			snprintf(endpoint, sizeof(endpoint), "%s/endpoint", target) >=
				(int)sizeof(endpoint) ||
			snprintf(source_lock, sizeof(source_lock), "%s/source.lock", endpoint) >=
				(int)sizeof(source_lock)) {
			errno = ENAMETOOLONG;
			return -1;
		}

		/* Contracts and lifetime locks are always exposed read-only. */
		if (move_mount_to_path(entry->channel_fd, target) != 0) {
			return report_mount_error("unable to expose channel at", target);
		}

		if (remount_bind(target, 1) != 0) {
			return report_mount_error("unable to make channel read-only at", target);
		}

		if ((entry->type == TGBS_CONTRACT_QUEUING &&
			    (entry->roles & CHANNEL_MOUNT_DESTINATION)) ||
			(entry->type == TGBS_CONTRACT_SAMPLING &&
				(entry->roles & CHANNEL_MOUNT_SOURCE))) {

			/* Queuing destinations create the socket; sampling sources will
			 * publish the latest value in the writable endpoint directory. */
			if (move_mount_to_path(entry->endpoint_fd, endpoint) != 0) {
				return report_mount_error(
					"unable to expose writable endpoint at", endpoint);
			}

			if (remount_bind(endpoint, 0) != 0) {
				return report_mount_error(
					"unable to make endpoint writable at", endpoint);
			}
		}

		/* Every participant sees source.lock through an explicit file mount.
		 * It is writable only when the domain also owns the source role. */
		if (move_mount_to_path(entry->source_lock_fd, source_lock) != 0) {
			return report_mount_error("unable to expose source lock at", source_lock);
		}

		if (remount_bind(source_lock, !(entry->roles & CHANNEL_MOUNT_SOURCE)) != 0) {
			return report_mount_error((entry->roles & CHANNEL_MOUNT_SOURCE)
					? "unable to make source lock writable at"
					: "unable to make source lock read-only at",
				source_lock);
		}
	}

	return 0;
}

static int setup_container_mounts(const struct channel_mount_plan *plan, struct rootfs_plan *rootfs,
	const struct bind_mount_plan *binds)
{

	/* Keep mounts created by the container from propagating to the host. */
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
		return -1;
	}

	if (rootfs_mount(rootfs) != 0) {
		return -1;
	}

	/* Match the host's volatile layout, including /var/log. /run was already
	 * prepared by rootfs_mount and must not be replaced here. */
	if (mount("tmpfs", "/var/volatile", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") != 0) {
		return report_mount_error(
			"unable to mount private volatile filesystem at", "/var/volatile");
	}

	if (mkdir_if_missing("/var/volatile", 0755) != 0 ||
		mkdir_if_missing("/var/volatile/tmp", 01777) != 0 ||
		mkdir_if_missing("/var/volatile/log", 0755) != 0) {
		return report_mount_error(
			"unable to prepare private volatile path at", "/var/volatile");
	}

	if (mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") != 0) {
		return report_mount_error("unable to mount private tmpfs at", "/tmp");
	}

	if (mkdir_if_missing("/run/lock", 0755) != 0) {
		return report_mount_error("unable to create private runtime path at", "/run/lock");
	}

	if (setup_channel_mounts(plan) != 0) {
		return -1;
	}

	/* Expose host device information without allowing sysfs configuration. */
	if (mount("sysfs", "/sys", "sysfs", MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) !=
		0) {
		return report_mount_error("unable to mount read-only sysfs at", "/sys");
	}

	/* In the new cgroup namespace this fresh mount is rooted at the domain. */
	if (mount("cgroup2", CG_ROOT, "cgroup2", MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV,
		    NULL) != 0) {
		return report_mount_error("unable to mount private cgroup view at", CG_ROOT);
	}

	/* Hide the inherited host procfs with one tied to this PID namespace. */
	if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
		return report_mount_error("unable to mount private procfs at", "/proc");
	}

	return bind_mount_plan_mount(binds);
}

static pid_t clone_container_into_cgroup(int cgroup_fd)
{
	struct clone_args args = {
		.flags = CLONE_INTO_CGROUP | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
			CLONE_NEWIPC | CLONE_NEWCGROUP,
		.exit_signal = SIGCHLD,
		.cgroup = (unsigned long long)cgroup_fd,
	};

	return (pid_t)syscall(SYS_clone3, &args, sizeof(args));
}

/* Remove only resources owned by this invocation. Preserve ownership metadata
 * if the cgroup cannot be removed, so observation can still report the orphan. */
static int cleanup_cgroup(const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
	for (int retry = 0; retry < 100; retry++) {
		if (rmdir(path) == 0 || errno == ENOENT) {
			return 0;
		}

		if (errno != EBUSY) {
			return -1;
		}

		if (cgroup_kill(name) != 0) {
			return -1;
		}

		usleep(10000);
	}

	errno = EBUSY;
	return -1;
}

static int cleanup_metadata(const char *name)
{
	char path[PATH_MAX];
	const char *files[] = { "main.pid", "main.pid.tmp" };

	for (size_t i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/%s/%s", RUN_ROOT, name, files[i]);
		if (unlink(path) != 0 && errno != ENOENT) {
			return -1;
		}
	}

	snprintf(path, sizeof(path), "%s/%s", RUN_ROOT, name);
	return rmdir(path);
}

static int write_meta(const char *name, pid_t pid, unsigned long long starttime)
{
	char temporary[PATH_MAX], final[PATH_MAX], text[128];
	int fd, result, saved;

	snprintf(temporary, sizeof(temporary), "%s/%s/main.pid.tmp", RUN_ROOT, name);
	snprintf(final, sizeof(final), "%s/%s/main.pid", RUN_ROOT, name);
	snprintf(text, sizeof(text), "%d %llu", (int)pid, starttime);
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}

	ssize_t n;
	size_t length = strlen(text);

	do {
		n = write(fd, text, length);
	} while (n < 0 && errno == EINTR);
	result = n == (ssize_t)length ? 0 : -1;
	saved = n < 0 ? errno : EIO;
	if (close(fd) != 0 && result == 0) {
		result = -1;
		saved = errno;
	}

	if (result == 0) {
		result = rename(temporary, final);
	} else {
		errno = saved;
	}

	return result;
}

int tgbs_domain_run(const tgbs_domain_spec_t *spec, tgbs_exit_status_t *status)
{
	struct rootfs_plan rootfs = { -1, -1, -1, -1, -1 };
	struct channel_mount_plan channel_mounts = { 0 };
	struct bind_mount_plan binds = { 0 };
	char path[PATH_MAX], memory_max[32], pids_max[32];
	int topology_fd = -1, cgroup_fd = -1, startup_fd = -1;
	int error_pipe[2] = { -1, -1 };
	int own_cgroup = 0, own_metadata = 0, failure = 0;
	pid_t pid = -1;
	unsigned long long starttime;
	uint64_t startup_token = 1;

	if (status == NULL) {
		errno = EINVAL;
		return -1;
	}

	memset(status, 0, sizeof(*status));
	if (spec == NULL || !valid_domain_name(spec->name) || spec->argv == NULL ||
		spec->argv[0] == NULL || !spec->argv[0][0] || spec->runtime_us == 0 ||
		spec->period_us == 0 || spec->runtime_us > spec->period_us || spec->reclaim < -1 ||
		spec->reclaim > 1 || (spec->ephemeral != 0 && spec->ephemeral != 1) ||
		(spec->bind_count && spec->binds == NULL) || spec->cancel_fd < -1 ||
		(spec->hostname && (!spec->hostname[0] || strlen(spec->hostname) > 64))) {
		errno = EINVAL;
		return -1;
	}

	if (limit_text(spec->memory_max, memory_max) != 0 ||
		limit_text(spec->pids_max, pids_max) != 0) {
		return -1;
	}

	if (spec->cancel_fd >= 0 && fcntl(spec->cancel_fd, F_GETFD) < 0) {
		return -1;
	}

	if (spec->cpus && strcmp(spec->cpus, "inherit") != 0) {
		struct cpu_list cpus = { 0 };

		if (parse_cpu_list(spec->cpus, &cpus) != 0) {
			return -1;
		}

		free_cpu_list(&cpus);
	}

	/* Prepare bind mounts and rootfs layers before creating the domain. */
	for (size_t i = 0; i < spec->bind_count; i++) {
		if (bind_mount_plan_add(&binds, &spec->binds[i]) != 0) {
			goto fail;
		}
	}

	if (verify_cgroup_env() != 0) {
		goto fail;
	}

	if (bind_mount_plan_prepare(&binds) != 0) {
		goto fail;
	}

	if (rootfs_plan_prepare(spec->name, spec->ephemeral, &rootfs) != 0) {
		goto fail;
	}

	/* Serialize startup with topology changes until the child consumes
	 * the detached channel mounts. */
	topology_fd = channel_topology_lock(1);
	if (topology_fd < 0) {
		goto fail;
	}

	if (channel_mount_plan_prepare(spec->name, &channel_mounts) != 0) {
		goto fail;
	}

	/* Track ownership so a failed launch removes only its own resources. */
	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, spec->name);
	if (mkdir(path, 0755) != 0) {
		goto fail;
	}

	own_cgroup = 1;
	snprintf(path, sizeof(path), "%s/%s", RUN_ROOT, spec->name);
	if (mkdir(path, 0755) != 0) {
		goto fail;
	}

	own_metadata = 1;

	/* Configure and verify resource controls before creating any tasks. */
	if (configure_domain_cpus(spec->name, spec->cpus ? spec->cpus : "inherit") != 0) {
		goto fail;
	}

	if (configure_domain_limit(spec->name, "memory.max", memory_max) != 0) {
		goto fail;
	}

	if (configure_domain_limit(spec->name, "pids.max", pids_max) != 0) {
		goto fail;
	}

	snprintf(path, sizeof(path), "%s/%s/cpu.period_us", CG_ROOT, spec->name);
	if (write_u64(path, spec->period_us) != 0) {
		goto fail;
	}

	unsigned long long actual;
	if (read_u64(path, &actual) != 0) {
		goto fail;
	}

	if (actual != spec->period_us) {
		errno = EIO;
		goto fail;
	}

	snprintf(path, sizeof(path), "%s/%s/cpu.runtime_us", CG_ROOT, spec->name);
	if (write_u64(path, spec->runtime_us) != 0) {
		goto fail;
	}

	struct runtime_config config = { 0 };

	if (read_runtime(path, &config) != 0) {
		goto fail;
	}

	actual = config.default_runtime;
	free_runtime(&config);
	if (actual != spec->runtime_us) {
		errno = EIO;
		goto fail;
	}

	if (spec->reclaim >= 0 && configure_domain_reclaim(spec->name, spec->reclaim) != 0) {
		goto fail;
	}

	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, spec->name);
	cgroup_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (cgroup_fd < 0) {
		goto fail;
	}

	if (pipe2(error_pipe, O_CLOEXEC) != 0) {
		goto fail;
	}

	startup_fd = eventfd(0, EFD_CLOEXEC);
	if (startup_fd < 0) {
		goto fail;
	}

	/* Create PID 1 directly in its configured cgroup and private
	 * namespaces. The supervising caller remains in the host namespaces. */
	pid = clone_container_into_cgroup(cgroup_fd);
	if (pid < 0) {
		goto fail;
	}

	if (pid == 0) {
		child_prepare();
		close(error_pipe[0]);
		close(cgroup_fd);
		ssize_t n;

		do {
			n = read(startup_fd, &startup_token, sizeof(startup_token));
		} while (n < 0 && errno == EINTR);
		if (n != sizeof(startup_token)) {
			child_fail(error_pipe[1], n < 0 ? errno : EIO);
		}

		close(startup_fd);
		if (setup_container_mounts(&channel_mounts, &rootfs, &binds) != 0) {
			child_fail(error_pipe[1], errno);
		}

		channel_mount_plan_close(&channel_mounts);
		bind_mount_plan_close(&binds);
		rootfs_plan_close(&rootfs);
		channel_topology_unlock(topology_fd);
		if (spec->hostname && sethostname(spec->hostname, strlen(spec->hostname)) != 0) {
			child_fail(error_pipe[1], errno);
		}

		if (drop_cap_sys_admin() != 0) {
			child_fail(error_pipe[1], errno);
		}

		execvp(spec->argv[0], spec->argv);
		child_fail(error_pipe[1], errno);
	}

	close(error_pipe[1]);
	error_pipe[1] = -1;
	close(cgroup_fd);
	cgroup_fd = -1;

	/* The child is gated: even a very short command has a published identity. */
	if (read_starttime(pid, &starttime) != 0 || write_meta(spec->name, pid, starttime) != 0) {
		goto fail;
	}

	if (write(startup_fd, &startup_token, sizeof(startup_token)) != sizeof(startup_token)) {
		goto fail;
	}

	close(startup_fd);
	startup_fd = -1;
	bind_mount_plan_close(&binds);

	/* Retain the exclusive storage lock through domain cleanup. */
	rootfs_plan_close_layers(&rootfs);
	channel_mount_plan_close(&channel_mounts);
	channel_topology_unlock(topology_fd);
	topology_fd = -1;

	/* Wait for exit or cancellation, then tear down the whole domain. */
	if (wait_child(pid, spec->cancel_fd, spec->name, error_pipe[0], SIGKILL, status) != 0) {
		failure = errno;
	}

	error_pipe[0] = -1;
	pid = -1;
	goto out;

fail:
	failure = errno ? errno : EIO;
	if (pid > 0) {
		cgroup_kill(spec->name);
		kill(pid, SIGKILL);
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
		}
	}

out:

	/* Preserve metadata when cgroup cleanup fails so observation can
	 * still identify resources left behind. */
	if (error_pipe[0] >= 0) {
		close(error_pipe[0]);
	}

	if (error_pipe[1] >= 0) {
		close(error_pipe[1]);
	}

	if (startup_fd >= 0) {
		close(startup_fd);
	}

	if (cgroup_fd >= 0) {
		close(cgroup_fd);
	}

	if (own_cgroup && cleanup_cgroup(spec->name) != 0) {
		status->cleanup_errno = errno;
	}

	if (!status->cleanup_errno && own_metadata && cleanup_metadata(spec->name) != 0) {
		status->cleanup_errno = errno;
	}

	channel_mount_plan_close(&channel_mounts);
	channel_topology_unlock(topology_fd);
	bind_mount_plan_close(&binds);
	rootfs_plan_close(&rootfs);
	if (!failure) {
		failure = status->cleanup_errno;
	}

	if (failure) {
		errno = failure;
		return -1;
	}

	return 0;
}
