/*
 * SPDX-License-Identifier: MIT
 *
 * Open a shell in an existing domain without changing its lifecycle.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"

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
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t shell_pid;

static void forward_signal(int sig)
{
	int saved_errno = errno;

	if (shell_pid > 0)
		kill((pid_t)shell_pid, sig);
	errno = saved_errno;
}

/* Use retained directory descriptors so teardown/recreation of NAME or reuse
 * of the leader PID cannot redirect a later open to a different domain. */
static FILE *open_stream_at(int dirfd, const char *name)
{
	int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
	FILE *f;

	if (fd < 0)
		return NULL;
	f = fdopen(fd, "r");
	if (f == NULL)
		close(fd);
	return f;
}

static int leader_in_cgroup(int cgroup_fd, pid_t pid)
{
	FILE *f = open_stream_at(cgroup_fd, "cgroup.procs");
	int member;
	int found = 0;

	if (f == NULL)
		return 0;
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

	if (f == NULL)
		return 0;
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

int cmd_attach(const char *name)
{
	char path[PATH_MAX];
	pid_t leader, child;
	unsigned long long recorded_start, current_start;
	int pid_fd = -1, proc_fd = -1, cgroup_fd = -1, root_fd = -1;
	int frozen, status, rc = 1;
	FILE *f;
	const char *operation = "open domain";
	struct clone_args args = {
		.flags = CLONE_INTO_CGROUP,
		.exit_signal = SIGCHLD,
	};
	struct sigaction sa = {0};
	sigset_t blocked, previous_mask;
	pid_t foreground;
	struct termios terminal;
	int have_terminal;

	if (!is_valid_name(name)) {
		fprintf(stderr, "tgbsctl: ERROR - invalid domain NAME '%s'\n", name);
		return 2;
	}
	if (read_main_meta(name, &leader, &recorded_start) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - no ready runtime metadata for %s\n", name);
		return 1;
	}

	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
	cgroup_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (cgroup_fd < 0)
		goto fail;
	pid_fd = (int)syscall(SYS_pidfd_open, leader, 0);
	if (pid_fd < 0)
		goto fail;
	snprintf(path, sizeof(path), "/proc/%d", (int)leader);
	proc_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (proc_fd < 0)
		goto fail;
	if (read_starttime(leader, &current_start) != 0 ||
	    current_start != recorded_start || !leader_in_cgroup(cgroup_fd, leader)) {
		fprintf(stderr, "tgbsctl: ERROR - main process for %s is no longer alive in its cgroup\n", name);
		goto out;
	}
	if (!leader_ready(proc_fd)) {
		fprintf(stderr, "tgbsctl: ERROR - domain %s is not ready; retry attach after startup\n", name);
		goto out;
	}
	operation = "read freezer state";
	f = open_stream_at(cgroup_fd, "cgroup.freeze");
	if (f == NULL)
		goto fail;
	status = fscanf(f, "%d", &frozen);
	fclose(f);
	if (status != 1) {
		errno = EIO;
		goto fail;
	}
	if (frozen != 0) {
		fprintf(stderr, "tgbsctl: ERROR - domain %s is paused; resume it before attaching\n", name);
		goto out;
	}
	operation = "open container root";
	root_fd = openat(proc_fd, "root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0)
		goto fail;

	/* A pidfd selects all namespaces atomically and stays tied to the verified
	 * leader. Joining a cgroup namespace alone does not change membership. */
	operation = "join container namespaces";
	if (setns(pid_fd, CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS |
			CLONE_NEWIPC | CLONE_NEWCGROUP) != 0)
		goto fail;
	operation = "enter container root";
	if (fchdir(root_fd) != 0 || chroot(".") != 0 || chdir("/") != 0)
		goto fail;
	close(root_fd);
	root_fd = -1;
	close(proc_fd);
	proc_fd = -1;
	close(pid_fd);
	pid_fd = -1;

	foreground = tcgetpgrp(STDIN_FILENO);
	have_terminal = tcgetattr(STDIN_FILENO, &terminal) == 0;
	/* Block signals across clone and handler installation. The waiting parent
	 * ignores terminal interrupts; the shell handles them on the inherited tty. */
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGQUIT);
	sigaddset(&blocked, SIGTERM);
	sigaddset(&blocked, SIGHUP);
	operation = "block attach signals";
	if (sigprocmask(SIG_BLOCK, &blocked, &previous_mask) != 0)
		goto fail;

	/* setns(CLONE_NEWPID) affects children only. clone3 creates the shell in
	 * both the target PID namespace and its resource-controlled cgroup. */
	args.cgroup = (unsigned long long)cgroup_fd;
	child = (pid_t)syscall(SYS_clone3, &args, sizeof(args));
	if (child < 0) {
		int saved_errno = errno;

		sigprocmask(SIG_SETMASK, &previous_mask, NULL);
		errno = saved_errno;
		operation = "create shell in container cgroup";
		goto fail;
	}
	if (child == 0) {
		char prompt[288];

		close(cgroup_fd);
		sa.sa_handler = SIG_DFL;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGQUIT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGHUP, &sa, NULL);
		sigprocmask(SIG_SETMASK, &previous_mask, NULL);
		if (drop_cap_sys_admin() != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to drop CAP_SYS_ADMIN: %s\n", strerror(errno));
			_exit(127);
		}
		snprintf(prompt, sizeof(prompt), "[tgbs:%s] # ", name);
		if (setenv("PS1", prompt, 1) != 0) {
			fprintf(stderr, "tgbsctl: ERROR - unable to set shell prompt: %s\n", strerror(errno));
			_exit(127);
		}
		execl("/bin/sh", "sh", (char *)NULL);
		fprintf(stderr, "tgbsctl: ERROR - unable to execute /bin/sh: %s\n", strerror(errno));
		_exit(127);
	}
	close(cgroup_fd);
	cgroup_fd = -1;
	shell_pid = child;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sa.sa_handler = forward_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigprocmask(SIG_SETMASK, &previous_mask, NULL);
	do {
		child = waitpid((pid_t)shell_pid, &status, 0);
	} while (child < 0 && errno == EINTR);
	shell_pid = 0;
	/* An interactive shell may have taken the terminal's foreground group. */
	sa.sa_handler = SIG_IGN;
	sigaction(SIGTTOU, &sa, NULL);
	if (foreground >= 0)
		tcsetpgrp(STDIN_FILENO, foreground);
	if (have_terminal)
		tcsetattr(STDIN_FILENO, TCSADRAIN, &terminal);
	if (child < 0) {
		operation = "wait for attached shell";
		goto fail;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) :
		WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;

fail:
	fprintf(stderr, "tgbsctl: ERROR - unable to %s for %s: %s\n",
		operation, name, strerror(errno));
out:
	if (root_fd >= 0)
		close(root_fd);
	if (proc_fd >= 0)
		close(proc_fd);
	if (pid_fd >= 0)
		close(pid_fd);
	if (cgroup_fd >= 0)
		close(cgroup_fd);
	return rc;
}
