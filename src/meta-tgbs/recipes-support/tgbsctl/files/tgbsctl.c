/*
 * SPDX-License-Identifier: MIT
 *
 * tgbsctl - command-line interface to the daemonless TGBS runtime.
 *
 * Dispatch container and channel commands through libtgbs. The CLI handles
 * arguments, output and signal policy; libtgbs owns runtime operations,
 * lifecycle supervision and cleanup.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s run --name NAME [--hostname HOSTNAME]\n"
		"         --runtime-us RUNTIME --period-us PERIOD\n"
		"         [--cpus CPU-LIST|inherit] [--reclaim BOOL]\n"
		"         [--memory-max BYTES|max] [--pids-max COUNT|max]\n"
		"         [--ephemeral]\n"
		"         [--bind SOURCE:DESTINATION[:ro|rw] ...]\n"
		"         COMMAND [ARGS...]\n"
		"  %s attach NAME\n"
		"  %s list\n"
		"  %s inspect NAME\n"
		"  %s kill NAME\n"
		"  %s pause NAME\n"
		"  %s resume NAME\n"
		"  %s set NAME runtime-us VALUE_US[,VALUE_US...]\n"
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
		"           cgroup namespaces, with a persistent overlay rootfs,\n"
		"           a static read-only base at /run/tgbs/rootfs/base,\n"
		"           private /tmp and /run mounts,\n"
		"           in a TGBS cgroup named NAME under %s.\n"
		"           Run options must follow 'run' and precede COMMAND.\n"
		"           --ephemeral uses a private tmpfs upper instead of persistent storage.\n"
		"           --bind shares an existing host file or directory at an absolute container path.\n"
		"           Binds are writable by default; ro makes the container mount read-only.\n"
		"           Missing destination paths are created without following symlinks.\n"
		"           /dev, /proc, /sys, " "/run/tgbs" ", " "/run/tgbs/channels" " and /run/lock\n"
		"           subtrees are reserved; / and /run cannot be replaced.\n"
		"           RUNTIME and PERIOD are in microseconds and must satisfy\n"
		"           0 < RUNTIME <= PERIOD.\n"
		"           CPU-LIST uses the cpuset list syntax, for example 0-1 or 0,2;\n"
		"           inherit selects the cgroup root's effective CPU list.\n"
		"           BOOL accepts 0, 1, false, or true.\n"
		"           Resource limits are positive integers or max (the default).\n"
		"           HOSTNAME is set in the private UTS namespace before privileges\n"
		"           are reduced. CAP_SYS_ADMIN is unavailable to COMMAND.\n"
		"  list     List the TGBS domains known to this runtime.\n"
		"  attach   Open /bin/sh in NAME's namespaces, rootfs, and TGBS cgroup.\n"
		"           Exiting this shell leaves the main process running.\n"
		"  inspect  Show the state, temporal contract, and processes for NAME.\n"
		"  kill     Kill every process in NAME. The run supervisor then cleans up.\n"
		"  pause    Freeze every process in NAME.\n"
		"  resume   Unfreeze every process in NAME.\n"
		"  set      Change one value of NAME's temporal contract.\n"
		"           runtime-us (alias runtime) accepts a default value or a comma-separated\n"
		"           list with one nonnegative value per possible CPU, in CPU ID order,\n"
		"           including offline CPUs. Lists preserve the default runtime.\n"
		"  channel  Manage immutable channel contracts between containers or the host.\n"
		"           host (alias system) denotes the host; both are reserved container names.\n"
		"           TYPE is required: queuing or sampling.\n"
		"           Queuing requires exactly one destination.\n"
		"           Sampling requires REFRESH_PERIOD in microseconds and allows\n"
		"           up to 64 distinct destinations.\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog, prog, prog, prog, prog, prog, "/sys/fs/cgroup");
}

static int dispatch(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "run") == 0)
		return cmd_run(argc, argv);
	if (strcmp(argv[1], "attach") == 0) {
		if (argc != 3) {
			fprintf(stderr, "tgbsctl: ERROR - attach requires exactly one NAME\n");
			usage(argv[0]);
			return 2;
		}
		return cmd_attach(argv[2]);
	}
	if (strcmp(argv[1], "list") == 0)
		return cmd_list();
	if (strcmp(argv[1], "inspect") == 0) {
		if (argc != 3) {
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
				"tgbsctl: ERROR - set requires NAME, runtime-us|runtime|period|cpus|reclaim|memory-max|pids-max, and VALUE\n");
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

int main(int argc, char **argv)
{
    return dispatch(argc, argv);
}
