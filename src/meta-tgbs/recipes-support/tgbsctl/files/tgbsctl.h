/*
 * Shared declarations for tgbsctl.
 *
 * cgroupfs under CG_ROOT is the single source of truth for resource and task
 * state. RUN_ROOT holds volatile domain ownership markers and immutable
 * communication-channel contracts. A main.pid marker records information the
 * kernel cannot rebuild: the forked leader PID plus its /proc
 * starttime, used to verify the main process identity across PID reuse.
 */

#ifndef TGBSCTL_H
#define TGBSCTL_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#ifndef CG_ROOT
#define CG_ROOT "/sys/fs/cgroup"
#endif

#ifndef RUN_ROOT
#define RUN_ROOT "/run/tgbs"
#endif

#define CPU_LIST_SIZE 4096
#ifndef CHANNEL_ROOT
#define CHANNEL_ROOT RUN_ROOT "/channels"
#endif

#define CHANNEL_TOPOLOGY_LOCK RUN_ROOT "/.channel-topology.lock"
#define CHANNEL_NAME_SIZE 64

/* Derived domain state. Never stored; recomputed from cgroupfs and /proc. */
enum domain_state {
	STATE_UNKNOWN = 0,
	STATE_INITIALIZING,
	STATE_RUNNING,
	STATE_PAUSED,
	STATE_ORPHAN,
	STATE_EXITED,
	STATE_STALE
};

enum channel_mount_role {
	CHANNEL_MOUNT_SOURCE = 1,
	CHANNEL_MOUNT_DESTINATION = 2
};

struct channel_mount_entry {
	char name[CHANNEL_NAME_SIZE];
	enum channel_mount_role role;
	int channel_fd;
	int endpoint_fd;
	int source_lock_fd;
};

struct channel_mount_plan {
	struct channel_mount_entry *entries;
	size_t count;
};

/* --- utilities defined in tgbsctl.c --- */

void usage(const char *prog);

int is_valid_name(const char *name);

int parse_positive(const char *text, unsigned long long *out);

/* Parse exactly 0/1 or true/false (case-insensitive). */
int parse_bool(const char *text, int *out);

void error_exit(const char *fmt, ...);

int write_u64(const char *path, unsigned long long value);

int read_u64(const char *path, unsigned long long *out);

/* cgroup string-file helpers. read_text strips trailing newlines. */
int write_text(const char *path, const char *value);

int read_text(const char *path, char *out, size_t outsz);

void verify_cgroup_env(void);

/* Kill every task in NAME, using cgroup.kill or a per-PID fallback. */
int cgroup_kill(const char *name);

/* Configure and verify cgroup-v2 controls shared by run and set. */
int configure_domain_cpus(const char *name, const char *cpu_list);

int configure_domain_reclaim(const char *name, int reclaim);

/* Read /proc/<pid>/stat starttime (kernel ticks). Returns 0 on success. */
int read_starttime(pid_t pid, unsigned long long *out);

/* --- command dispatch --- */

int cmd_run(int argc, char **argv);
int cmd_list(void);
int cmd_inspect(const char *name);
int cmd_kill(const char *name);
int cmd_freeze(const char *name, int freeze);
int cmd_set(const char *name, const char *field, const char *value_text);
int cmd_channel(int argc, char **argv);

/* --- channel topology helpers defined in channel.c --- */

/* Serialize channel topology changes with domain startup. The returned file
 * descriptor owns the lock and must be closed with channel_topology_unlock(). */
int channel_topology_lock(int exclusive);
void channel_topology_unlock(int fd);

/* Validate the channel topology and retain detached mount descriptors for
 * every channel attached to DOMAIN. The topology lock must remain held until
 * the descriptors have been consumed by the child mount namespace. */
int channel_mount_plan_prepare(const char *domain,
	struct channel_mount_plan *plan);
void channel_mount_plan_close(struct channel_mount_plan *plan);

/* --- observe helpers defined in observe.c (read-only) --- */

const char *state_name(enum domain_state s);

#endif
