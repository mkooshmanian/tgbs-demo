/*
 * Private runtime declarations; never installed as public headers.
 *
 * cgroupfs under CG_ROOT is the single source of truth for resource and task
 * state. RUN_ROOT holds volatile domain ownership markers and immutable
 * communication-channel contracts. A main.pid marker records information the
 * kernel cannot rebuild: the forked leader PID plus its /proc
 * starttime, used to verify the main process identity across PID reuse.
 */

#ifndef TGBS_RUNTIME_INTERNAL_H
#define TGBS_RUNTIME_INTERNAL_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include "domain.h"
#include "channel.h"
#include "channel-contract.h"
#include "cpu-runtime.h"

#ifndef CG_ROOT
#define CG_ROOT "/sys/fs/cgroup"
#endif

#ifndef RUN_ROOT
#define RUN_ROOT "/run/tgbs"
#endif

/* Original image mount retained by tgbs-preinit, never the host overlay. */
#define ROOTFS_LOWER RUN_ROOT "/rootfs/base"
#define ROOTFS_STORE "/var/lib/tgbs"

struct rootfs_plan {
	int lower_fd;
	int upper_fd;
	int work_fd;
	int domain_fd;
	int lock_fd;
};

int rootfs_plan_prepare(const char *name, int ephemeral, struct rootfs_plan *plan);
void rootfs_plan_close_layers(struct rootfs_plan *plan);
void rootfs_plan_close(struct rootfs_plan *plan);
int rootfs_mount(struct rootfs_plan *plan);

struct bind_mount_entry {
	char *source;
	char *destination;
	int read_only;
	int is_directory;
	int mount_fd;
};

struct bind_mount_plan {
	struct bind_mount_entry *entries;
	size_t count;
};

int bind_mount_plan_add(struct bind_mount_plan *plan, const tgbs_bind_t *spec);
int bind_mount_plan_prepare(struct bind_mount_plan *plan);
int bind_mount_plan_mount(const struct bind_mount_plan *plan);
void bind_mount_plan_close(struct bind_mount_plan *plan);

#define CPU_LIST_SIZE 4096
#ifndef CHANNEL_ROOT
#define CHANNEL_ROOT RUN_ROOT "/channels"
#endif

#define CHANNEL_TOPOLOGY_LOCK RUN_ROOT "/.channel-topology.lock"
#define CHANNEL_NAME_SIZE TGBS_CONTRACT_NAME_SIZE

enum channel_mount_role {
	CHANNEL_MOUNT_SOURCE = 1U << 0,
	CHANNEL_MOUNT_DESTINATION = 1U << 1
};

struct channel_mount_entry {
	char name[CHANNEL_NAME_SIZE];
	enum tgbs_contract_type type;
	unsigned int roles;
	int channel_fd;
	int endpoint_fd;
	int source_lock_fd;
};

struct channel_mount_plan {
	struct channel_mount_entry *entries;
	size_t count;
};

int is_valid_name(const char *name);
int valid_domain_name(const char *name);
int parse_positive(const char *text, unsigned long long *out);
int parse_cgroup_limit(const char *text, char *out, size_t outsz);
int write_u64(const char *path, unsigned long long value);
int read_u64(const char *path, unsigned long long *out);
int write_text(const char *path, const char *value);
int read_text(const char *path, char *out, size_t outsz);
int verify_cgroup_env(void);
int cgroup_kill(const char *name);
int configure_domain_cpus(const char *name, const char *cpu_list);
int configure_domain_reclaim(const char *name, int reclaim);
int configure_domain_limit(const char *name, const char *filename, const char *value);
int limit_text(tgbs_limit_t limit, char text[32]);
int read_starttime(pid_t pid, unsigned long long *out);
int read_main_meta(const char *run_root, const char *name, pid_t *pid,
                   unsigned long long *starttime);
int drop_cap_sys_admin(void);
int channel_topology_lock(int exclusive);
void channel_topology_unlock(int fd);
int channel_mount_plan_prepare(const char *domain,
                               struct channel_mount_plan *plan);
void channel_mount_plan_close(struct channel_mount_plan *plan);
void child_fail(int fd, int error);
void child_prepare(void);
int wait_child(pid_t pid, int cancel_fd, const char *domain, int error_fd, int cancel_signal,
                tgbs_exit_status_t *status);
int domain_path(const char *cgroup_root, const char *name, const char *file,
                 char path[PATH_MAX]);
#endif
