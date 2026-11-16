/*
 * SPDX-License-Identifier: MIT
 *
 * Execute a command in an existing domain without changing its lifecycle.
 *
 * This is the runtime part of the former tgbsctl attach implementation.
 * Retain and verify the leader's identity, then enter its namespaces and root
 * in a child so the caller keeps its own execution environment.
 */

#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t shell_pid;

static void forward_signal(int sig)
{
	int saved_errno = errno;

	(void)sig;

	if (shell_pid > 0) {
		kill((pid_t)shell_pid, SIGKILL);
	}

	errno = saved_errno;
}

/* Use retained directory descriptors so teardown/recreation of NAME or reuse
 * of the leader PID cannot redirect a later open to a different domain. */
static FILE *open_stream_at(int dirfd, const char *name)
{
	int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
	FILE *f;

	if (fd < 0) {
		return NULL;
	}

	f = fdopen(fd, "r");
	if (f == NULL) {
		close(fd);
	}

	return f;
}

static int leader_in_cgroup(int cgroup_fd, pid_t pid)
{
	FILE *f = open_stream_at(cgroup_fd, "cgroup.procs");
	int member;
	int found = 0;

	if (f == NULL) {
		return 0;
	}
	while (fscanf(f, "%d", &member) == 1) {
		if (member == pid) {
			found = 1;
			break;
		}
	}

	fclose(f);
	return found;
}

/* run publishes main.pid before completing mounts. no_new_privs is set only
 * after mount setup and hostname configuration, immediately before exec. */
static int leader_ready(int proc_fd)
{
	FILE *f = open_stream_at(proc_fd, "status");
	char line[256];
	int ready = 0;

	if (f == NULL) {
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		int value;

		if (sscanf(line, "NoNewPrivs: %d", &value) == 1) {
			ready = value == 1;
			break;
		}
	}

	fclose(f);
	return ready;
}

int tgbs_domain_exec(const char *name, const tgbs_exec_spec_t *spec, tgbs_exit_status_t *status)
{
	char path[PATH_MAX];
	pid_t leader;
	unsigned long long recorded_start, current_start;
	int pid_fd = -1, proc_fd = -1, cgroup_fd = -1, root_fd = -1;
	int error_pipe[2] = { -1, -1 }, failure = 0;
	pid_t helper = -1;

	if (status == NULL) {
		errno = EINVAL;
		return -1;
	}

	memset(status, 0, sizeof(*status));
	if (!valid_domain_name(name) || spec == NULL || spec->argv == NULL ||
		spec->argv[0] == NULL || !spec->argv[0][0] || spec->cancel_fd < -1) {
		errno = EINVAL;
		return -1;
	}

	if (spec->cancel_fd >= 0 && fcntl(spec->cancel_fd, F_GETFD) < 0) {
		return -1;
	}

	/* Pin and verify the recorded leader before using its namespaces. */
	if (read_main_meta(RUN_ROOT, name, &leader, &recorded_start) != 0) {
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
	cgroup_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (cgroup_fd < 0) {
		goto fail;
	}

	pid_fd = (int)syscall(SYS_pidfd_open, leader, 0);
	if (pid_fd < 0) {
		goto fail;
	}

	snprintf(path, sizeof(path), "/proc/%d", (int)leader);
	proc_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (proc_fd < 0) {
		goto fail;
	}

	if (read_starttime(leader, &current_start) != 0) {
		goto fail;
	}

	if (current_start != recorded_start || !leader_in_cgroup(cgroup_fd, leader)) {
		errno = ESTALE;
		goto fail;
	}

	if (!leader_ready(proc_fd)) {
		errno = EAGAIN;
		goto fail;
	}

	/* A frozen domain cannot run the attached command. */
	FILE *file = open_stream_at(cgroup_fd, "cgroup.freeze");

	if (file == NULL) {
		goto fail;
	}

	int frozen = -1;
	int parsed = fscanf(file, "%d", &frozen);

	fclose(file);
	if (parsed != 1) {
		errno = EPROTO;
		goto fail;
	}

	if (frozen) {
		errno = EBUSY;
		goto fail;
	}

	root_fd = openat(proc_fd, "root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0) {
		goto fail;
	}

	if (pipe2(error_pipe, O_CLOEXEC) != 0) {
		goto fail;
	}

	/* Keep all namespace and root changes out of the calling process. */
	helper = fork();
	if (helper < 0) {
		goto fail;
	}

	if (helper == 0) {
		child_prepare();
		close(error_pipe[0]);

		/* This intermediary alone changes namespaces and root. It is outside
		 * the target PID namespace; its next child enters that namespace. */
		if (setns(pid_fd,
			    CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC |
				    CLONE_NEWCGROUP) != 0) {
			child_fail(error_pipe[1], errno);
		}

		if (fchdir(root_fd) != 0 || chroot(".") != 0 || chdir("/") != 0) {
			child_fail(error_pipe[1], errno);
		}

		close(root_fd);
		close(proc_fd);
		close(pid_fd);

		/* Block termination signals across clone and child PID publication.
		 * The command handles terminal interrupts through inherited stdio. */
		sigset_t blocked;

		sigemptyset(&blocked);
		sigaddset(&blocked, SIGTERM);
		sigaddset(&blocked, SIGHUP);
		sigprocmask(SIG_BLOCK, &blocked, NULL);
		struct sigaction action = { .sa_handler = SIG_IGN };

		sigemptyset(&action.sa_mask);
		sigaction(SIGINT, &action, NULL);
		sigaction(SIGQUIT, &action, NULL);
		action.sa_handler = forward_signal;
		sigaction(SIGTERM, &action, NULL);
		sigaction(SIGHUP, &action, NULL);

		/* setns(CLONE_NEWPID) affects children only. Create the command
		 * in both the selected PID namespace and the domain's cgroup. */
		struct clone_args args = {
			.flags = CLONE_INTO_CGROUP,
			.exit_signal = SIGCHLD,
			.cgroup = (unsigned long long)cgroup_fd,
		};

		pid_t child = (pid_t)syscall(SYS_clone3, &args, sizeof(args));

		if (child < 0) {
			child_fail(error_pipe[1], errno);
		}

		if (child == 0) {
			child_prepare();
			close(cgroup_fd);

			/* The command's parent is outside its PID namespace, so getppid
			 * may be zero. PDEATHSIG still ties it to this intermediary. */
			if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
				child_fail(error_pipe[1], errno);
			}

			if (drop_cap_sys_admin() != 0) {
				child_fail(error_pipe[1], errno);
			}

			execvp(spec->argv[0], spec->argv);
			child_fail(error_pipe[1], errno);
		}

		close(cgroup_fd);
		close(error_pipe[1]);

		/* This child only waits now; do not retain application descriptors. */
		syscall(SYS_close_range, 3U, ~0U, 0);
		shell_pid = child;
		sigprocmask(SIG_UNBLOCK, &blocked, NULL);
		int child_status;
		pid_t waited_pid;

		do {
			waited_pid = waitpid(child, &child_status, 0);
		} while (waited_pid < 0 && errno == EINTR);
		if (waited_pid < 0) {
			_exit(125);
		}

		if (WIFEXITED(child_status)) {
			_exit(WEXITSTATUS(child_status));
		}

		int sig = WTERMSIG(child_status);

		action.sa_handler = SIG_DFL;
		sigaction(sig, &action, NULL);
		kill(getpid(), sig);
		_exit(128 + sig);
	}

	close(error_pipe[1]);
	error_pipe[1] = -1;
	if (wait_child(helper, spec->cancel_fd, NULL, error_pipe[0], SIGTERM, status) != 0) {
		failure = errno;
	}

	error_pipe[0] = -1;
	goto out;

fail:
	failure = errno;

out:
	if (pid_fd >= 0) {
		close(pid_fd);
	}

	if (proc_fd >= 0) {
		close(proc_fd);
	}

	if (cgroup_fd >= 0) {
		close(cgroup_fd);
	}

	if (root_fd >= 0) {
		close(root_fd);
	}

	if (error_pipe[0] >= 0) {
		close(error_pipe[0]);
	}

	if (error_pipe[1] >= 0) {
		close(error_pipe[1]);
	}

	if (failure) {
		errno = failure;
		return -1;
	}

	return 0;
}
