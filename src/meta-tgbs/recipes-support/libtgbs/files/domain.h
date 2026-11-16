/* SPDX-License-Identifier: MIT */
#ifndef TGBS_DOMAIN_H
#define TGBS_DOMAIN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Functions returning int use 0 for success or -1 with errno on failure.
 * Run and exec require a single-threaded caller, own/reap their children, and
 * install no signal handlers. Callers must not reap those children or use
 * SIGCHLD=SIG_IGN/SA_NOCLDWAIT. */

#define TGBS_DOMAIN_NAME_SIZE 256
#define TGBS_CPU_LIST_SIZE 4096

typedef struct {
    uint64_t value;
    int unlimited;
} tgbs_limit_t;

typedef struct {
    const char *source;
    const char *destination;
    int read_only;
} tgbs_bind_t;

typedef struct {
    const char *name;
    const char *hostname;
    char *const *argv;           /* NULL-terminated, executed with execvp. */
    uint64_t runtime_us;
    uint64_t period_us;
    const char *cpus;            /* cpuset syntax; NULL means inherit. */
    int reclaim;                /* -1 preserves the kernel default. */
    tgbs_limit_t memory_max;
    tgbs_limit_t pids_max;
    int ephemeral;
    const tgbs_bind_t *binds;
    size_t bind_count;
    int cancel_fd;              /* -1 disables; readable/HUP requests kill. */
} tgbs_domain_spec_t;

#define TGBS_DOMAIN_SPEC_INIT { .reclaim = -1, \
    .memory_max = { .unlimited = 1 }, .pids_max = { .unlimited = 1 }, \
    .cancel_fd = -1 }

typedef struct {
    int exited;
    int exit_code;
    int term_signal;
    int cancelled;
    int cleanup_errno;          /* Kept separately from a primary failure. */
} tgbs_exit_status_t;

/* Input pointers are borrowed until return. The caller supervises the domain
 * throughout this blocking call, retaining its storage lock until cleanup.
 * A child command's nonzero exit is a successful runtime operation. Setup or
 * exec failure returns -1, distinct from the command exiting with code 127.
 * On error status is still initialized; cleanup_errno reports failed cleanup.
 * Cancellation kills the entire cgroup and reaps the child before return.
 * The descriptor is borrowed and never read or closed by the library.
 * EINTR alone does not cancel. The API cannot clean up after caller SIGKILL.
 */
int tgbs_domain_run(const tgbs_domain_spec_t *spec, tgbs_exit_status_t *status);

typedef struct {
    char *const *argv;
    int cancel_fd;
} tgbs_exec_spec_t;
#define TGBS_EXEC_SPEC_INIT { .cancel_fd = -1 }

/* Enter namespaces/rootfs only in a child. Inherits the caller's stdio. This
 * is a synchronous command execution API, without PTY allocation/job control.
 * Cancellation kills this execution, leaving the main domain alive. */
int tgbs_domain_exec(const char *name, const tgbs_exec_spec_t *spec,
                     tgbs_exit_status_t *status);

int tgbs_domain_kill(const char *name);
int tgbs_domain_pause(const char *name);
int tgbs_domain_resume(const char *name);
int tgbs_domain_set_runtime(const char *name, uint64_t runtime_us);
/* One value per possible CPU, including offline CPUs, in ascending ID order.
 * Preserves the default budget. Best-effort rollback on partial failure. */
int tgbs_domain_set_cpu_runtimes(const char *name, const uint64_t *values,
                                 size_t count);
int tgbs_domain_set_period(const char *name, uint64_t period_us);
int tgbs_domain_set_cpus(const char *name, const char *cpus);
int tgbs_domain_set_reclaim(const char *name, int reclaim);
int tgbs_domain_set_memory_max(const char *name, tgbs_limit_t limit);
int tgbs_domain_set_pids_max(const char *name, tgbs_limit_t limit);

typedef enum {
    TGBS_DOMAIN_UNKNOWN = 0,
    TGBS_DOMAIN_INITIALIZING,
    TGBS_DOMAIN_RUNNING,
    TGBS_DOMAIN_PAUSED,
    TGBS_DOMAIN_ORPHAN,
    TGBS_DOMAIN_EXITED,
    TGBS_DOMAIN_STALE
} tgbs_domain_state_t;

typedef struct {
    unsigned int cpu;
    uint64_t runtime_us;
    int enabled;
} tgbs_cpu_budget_t;

enum {
    TGBS_INFO_CONTRACT = 1U << 0,
    TGBS_INFO_CPUS = 1U << 1,
    TGBS_INFO_RECLAIM = 1U << 2,
    TGBS_INFO_USAGE = 1U << 3,
    TGBS_INFO_MEMORY_CURRENT = 1U << 4,
    TGBS_INFO_MEMORY_PEAK = 1U << 5,
    TGBS_INFO_MEMORY_MAX = 1U << 6,
    TGBS_INFO_PIDS_CURRENT = 1U << 7,
    TGBS_INFO_PIDS_MAX = 1U << 8
};

typedef struct {
    char name[TGBS_DOMAIN_NAME_SIZE];
    tgbs_domain_state_t state;
    pid_t main_pid;
    uint64_t main_starttime;
    unsigned int available;     /* Optional measurements valid in snapshot. */
    uint64_t default_runtime_us;
    uint64_t period_us;
    char cpus[TGBS_CPU_LIST_SIZE];
    tgbs_cpu_budget_t *cpu_budgets;
    size_t cpu_count;
    long double total_budget_percent;
    int reclaim;
    uint64_t usage_us;
    uint64_t memory_current;
    uint64_t memory_peak;
    tgbs_limit_t memory_max;
    uint64_t pids_current;
    tgbs_limit_t pids_max;
    pid_t *processes;
    size_t process_count;
    pid_t *threads;
    size_t thread_count;
    char *command;
} tgbs_domain_info_t;

/* Snapshots may race with exit/control. Missing optional measurements are
 * represented by available flags, never by fabricated zero measurements.
 * Owned results must be freed with the matching library function. Output
 * pointers are set to NULL on failure. A missing marker directory is ENOENT. */
int tgbs_domain_inspect(const char *name, tgbs_domain_info_t **info);
void tgbs_domain_info_free(tgbs_domain_info_t *info);
int tgbs_domain_list(tgbs_domain_info_t ***domains, size_t *count);
/* Read-only listing with alternate roots, used by tgbs-top. NULL selects
 * the standard path for either root. Paths are borrowed until return. */
int tgbs_domain_list_at(const char *cgroup_root, const char *run_root,
                        tgbs_domain_info_t ***domains, size_t *count);
void tgbs_domain_list_free(tgbs_domain_info_t **domains, size_t count);
const char *tgbs_domain_state_name(tgbs_domain_state_t state);

#ifdef __cplusplus
}
#endif
#endif
