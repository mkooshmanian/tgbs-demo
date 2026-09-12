/**
 * @file fake-task.c
 * @brief Fake task simulating a periodic or continuous activation.
 *
 * This program simulates a task with:
 *  - A periodic activation mode driven by an absolute period T (milliseconds), or
 *  - A continuous activation mode where jobs are chained back-to-back without idle time.
 *  - A fixed or randomized execution budget C (milliseconds) per job, measured in
 *    thread CPU time (CLOCK_THREAD_CPUTIME_ID), so preemption by the OS does not
 *    reduce the consumed budget.
 *
 * Each loop iteration performs a single "spin" work step. Job start/end boundaries
 * are emitted as trace records sent over a Unix datagram socket, configured with
 * --metrics-socket, so several fake-task instances can share the same socket and
 * a single collector can read all of them.
 *
 * Scheduling policy (fair/fifo/rr/dl), niceness, RT priority and deadline
 * parameters are selectable via CLI options.
 *
 * Examples:
 *   ./fake-task --periodic 1000 200                # Periodic: T=1000 ms, C=200 ms
 *   ./fake-task --continuous 5                     # Continuous: C=5 ms, infinite jobs
 *   ./fake-task --periodic -v 1000 200 10          # Periodic verbose mode, 10 iterations
 *   ./fake-task --periodic --rand-exec 1000 200 10 # Random C for every job, WCET=200 ms
 *   ./fake-task --periodic --log=out.csv 50 5 100  # Log per-iteration metrics to CSV
 *   ./fake-task --periodic -s rr -p 10 1000 200    # Round-robin, priority 10, periodic task
 *   ./fake-task --periodic --metrics-socket=/tmp/metrics.sock 1000 200
 *   ./fake-task --periodic --metrics-socket=/tmp/metrics.sock --spin 1000 200
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <sys/resource.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/*
 * Do not include <linux/sched/types.h> here: both that kernel UAPI header and
 * the libc <sched.h> header define struct sched_param on some toolchains.
 * sched_attr has no libc definition, so keep the syscall ABI declaration
 * local.  The first 48 bytes are SCHED_ATTR_SIZE_VER0; the final fields are
 * the optional utilization hints added by SCHED_ATTR_SIZE_VER1.
 */
struct sched_attr
{
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
    uint32_t sched_util_min;
    uint32_t sched_util_max;
};

#ifndef SYS_sched_setattr
#ifdef __NR_sched_setattr
#define SYS_sched_setattr __NR_sched_setattr
#endif
#endif

/// Convert milliseconds to nanoseconds (macro version).
#define NS_FROM_MS(ms) ((ms) * 1000000LL)

/*
 * Random execution-time distribution:
 *   C = Cmin + (Cmax - Cmin) * X, X ~ Beta(a, b)
 *   Cmin = WCET * RAND_EXEC_MIN_FACTOR
 *   Cmax = WCET * RAND_EXEC_MAX_FACTOR
 */
#define RAND_EXEC_BETA_A 2.5
#define RAND_EXEC_BETA_B 5.0
#define RAND_EXEC_MIN_FACTOR 0.5
#define RAND_EXEC_MAX_FACTOR 1.0

/// Global flag for verbose logging.
static bool g_verbose = false;

typedef enum
{
    ACT_MODE_PERIODIC = 0,
    ACT_MODE_CONTINUOUS
} activation_mode_t;

static const char *activation_mode_str(activation_mode_t mode)
{
    switch (mode)
    {
    case ACT_MODE_PERIODIC:
        return "periodic";
    case ACT_MODE_CONTINUOUS:
        return "continuous";
    default:
        return "unknown";
    }
}

/*** Execution-time sampling ******************************************/

static unsigned long long g_rand_exec_state;
static bool g_rand_exec_seeded;

static unsigned long long rand_exec_u64(void)
{
    if (!g_rand_exec_seeded)
    {
        struct timespec realtime;
        struct timespec monotonic;
        clock_gettime(CLOCK_REALTIME, &realtime);
        clock_gettime(CLOCK_MONOTONIC, &monotonic);
        g_rand_exec_state =
            ((unsigned long long)realtime.tv_sec << 32) ^
            (unsigned long long)realtime.tv_nsec ^
            ((unsigned long long)monotonic.tv_sec << 17) ^
            (unsigned long long)monotonic.tv_nsec ^
            ((unsigned long long)getpid() << 1);
        g_rand_exec_seeded = true;
    }

    unsigned long long z = (g_rand_exec_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/** @return A uniform random value strictly between 0 and 1. */
static double rand_exec_uniform_open01(void)
{
    const double inv_2_53 = 1.0 / 9007199254740992.0;
    return ((double)(rand_exec_u64() >> 11) + 0.5) * inv_2_53;
}

/** @return A standard normal variate generated with the Box-Muller transform. */
static double rand_exec_standard_normal(void)
{
    const double two_pi = 6.283185307179586476925286766559;
    const double u1 = rand_exec_uniform_open01();
    const double u2 = rand_exec_uniform_open01();
    return sqrt(-2.0 * log(u1)) * cos(two_pi * u2);
}

/**
 * @brief Sample Gamma(shape, 1) using the Marsaglia-Tsang method.
 *
 * The shape < 1 reduction keeps the function valid if the Beta parameters are
 * changed later.
 */
static double rand_exec_gamma(double shape)
{
    if (shape < 1.0)
    {
        return rand_exec_gamma(shape + 1.0) *
               pow(rand_exec_uniform_open01(), 1.0 / shape);
    }

    const double d = shape - (1.0 / 3.0);
    const double c = 1.0 / sqrt(9.0 * d);

    for (;;)
    {
        const double x = rand_exec_standard_normal();
        double v = 1.0 + c * x;
        if (v <= 0.0)
            continue;
        v = v * v * v;

        const double u = rand_exec_uniform_open01();
        const double x2 = x * x;
        if (u < 1.0 - 0.0331 * x2 * x2 ||
            log(u) < 0.5 * x2 + d * (1.0 - v + log(v)))
        {
            return d * v;
        }
    }
}

/** @return A Beta(a, b) variate obtained from two Gamma variates. */
static double rand_exec_beta(double a, double b)
{
    const double gamma_a = rand_exec_gamma(a);
    const double gamma_b = rand_exec_gamma(b);
    return gamma_a / (gamma_a + gamma_b);
}

/**
 * @brief Select the CPU budget for one job.
 *
 * @param wcet_ms   WCET supplied on the command line, in milliseconds.
 * @param randomize Whether to draw a randomized execution time.
 * @return Selected execution budget in nanoseconds.
 */
static int64_t select_exec_budget_ns(int64_t wcet_ms, bool randomize)
{
    const double wcet_ns = (double)wcet_ms * 1000000.0;
    if (!randomize || wcet_ms == 0)
        return (int64_t)wcet_ns;

    const double x = rand_exec_beta(RAND_EXEC_BETA_A, RAND_EXEC_BETA_B);
    const double factor = RAND_EXEC_MIN_FACTOR +
                          (RAND_EXEC_MAX_FACTOR - RAND_EXEC_MIN_FACTOR) * x;
    return (int64_t)llround(wcet_ns * factor);
}

/*** Time helpers *********************************************************/

static inline int64_t now_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline int64_t thread_cpu_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline void timespec_from_abs_ns(int64_t ns, struct timespec *out_ts)
{
    out_ts->tv_sec = ns / 1000000000LL;
    out_ts->tv_nsec = ns % 1000000000LL;
}

/**
 * @brief Sleep until an absolute monotonic timestamp.
 *
 * Uses clock_nanosleep with TIMER_ABSTIME on CLOCK_MONOTONIC. If interrupted by a signal,
 * it retries until the absolute deadline is reached or passed.
 *
 * @param abs_ns Absolute wake-up time in nanoseconds (CLOCK_MONOTONIC domain).
 */
static void sleep_until_abs_monotonic_ns(int64_t abs_ns)
{
    struct timespec ts;
    timespec_from_abs_ns(abs_ns, &ts);
    for (;;)
    {
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        if (rc == 0)
            return; // Woke up at/after deadline
        if (rc == EINTR)
            continue; // Interrupted, retry same absolute time
        break;
    }
}

/*** Metrics over a Unix datagram socket **********************************/

static int metrics_fd = -1;
static struct sockaddr_un metrics_dst;
static socklen_t metrics_dst_len;

/*
 * On-wire metrics record: one datagram per job, little-endian.
 *
 *   magic     : 0x004A4B46 (magic + version nibble)
 *   version   : 0x00000001
 *   type      : 0 = T0 (loop start), 1 = job datagram
 *   pid       : 32-bit
 *   iter      : 32-bit index of the job
 *   start_ns  : 64-bit monotonic instant when the job started
 *   finish_ns : 64-bit monotonic instant when the job finished
 *
 * The collector relies on the fixed 36-byte layout; it need not know the
 * emitter's page size for integer fields because the struct is written
 * byte-for-byte (native on the little-endian x86/ARM test hosts; swap the
 * integer fields explicitly on a big-endian collector).
 */
#define FAKEJOB_MAGIC    0x004A4B46u
#define FAKEJOB_VERSION  1u
#define FAKEJOB_TOTAL_SIZE  36

/* record kind carried by each datagram */
#define FAKEJOB_TYPE_T0    0
#define FAKEJOB_TYPE_JOB   1

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t pid;
    uint32_t iter;
    uint64_t start_ns;
    uint64_t finish_ns;
} metrics_record_t;
#pragma pack(pop)
_Static_assert(sizeof(metrics_record_t) == FAKEJOB_TOTAL_SIZE,
               "metrics_record_t must be exactly FAKEJOB_TOTAL_SIZE bytes");

/**
 * @brief Open a Unix datagram socket for best-effort metrics.
 *
 * @param path Socket path (abstract or pathname socket).
 * @return true on success, false on error.
 */
static bool metrics_socket_open(const char *path)
{
    if (path == NULL || *path == '\0')
        return false;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("metrics: socket");
        return false;
    }

    memset(&metrics_dst, 0, sizeof(metrics_dst));
    metrics_dst.sun_family = AF_UNIX;

    bool is_abstract = (path[0] == '@');
    const char *socket_name = is_abstract ? path + 1 : path;
    size_t len = strlen(socket_name);
    if (is_abstract)
    {
        /* NUL prefix + name for abstract sockets */
        if (len == 0 || len >= sizeof(metrics_dst.sun_path) - 1)
        {
            fprintf(stderr, "metrics: abstract socket name too long\n");
            close(fd);
            return false;
        }
        metrics_dst.sun_path[0] = '\0';
        memcpy(&metrics_dst.sun_path[1], socket_name, len);
        metrics_dst_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len);
    }
    else
    {
        if (len + 1 >= sizeof(metrics_dst.sun_path))
        {
            fprintf(stderr, "metrics: socket path too long\n");
            close(fd);
            return false;
        }
        memcpy(metrics_dst.sun_path, path, len);
        metrics_dst.sun_path[len] = '\0';
        metrics_dst_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len + 1);
    }

    metrics_fd = fd;
    return true;
}

/**
 * @brief Emit a single FAKEJOB record per job over the datagram socket.
 *
 * The record is a fixed-size, little-endian struct (see metrics_record_t above):
 * one datagram per job carrying pid, iter, start_ns and finish_ns.
 *
 * @param iter     Iteration index.
 * @param start_ns Monotonic instant when the job started.
 * @param finish_ns Monotonic instant when the job finished.
 */
static inline void metrics_emit_job(uint64_t iter, int64_t start_ns, int64_t finish_ns)
{
    if (metrics_fd < 0)
        return;

    metrics_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = FAKEJOB_MAGIC;
    rec.version = FAKEJOB_VERSION;
    rec.type = FAKEJOB_TYPE_JOB;
    rec.pid = (uint32_t)getpid();
    rec.iter = (uint32_t)iter;
    rec.start_ns = (uint64_t)start_ns;
    rec.finish_ns = (uint64_t)finish_ns;

    char buf[FAKEJOB_TOTAL_SIZE];
    memcpy(buf, &rec, sizeof(rec));

    /* datagrams are best-effort; our records are tiny (well under typical
     * limits, ~8 KiB), so a single write is safe. */
    ssize_t sent = sendto(metrics_fd, buf, FAKEJOB_TOTAL_SIZE, MSG_NOSIGNAL,
                          (struct sockaddr *)&metrics_dst, metrics_dst_len);
    (void)sent;
}

/**
 * @brief Emit a T0 datagram recording the loop's first release instant.
 *
 * The collector needs the absolute instant of the first release to rebuild
 * the release-time schedule (and therefore lateness / response times). This
 * datagram is sent once, right after the socket is opened and before any job
 * datagram.
 *
 * @param release_ns First release instant in monotonic nanoseconds.
 */
static inline void metrics_emit_t0(int64_t release_ns)
{
    if (metrics_fd < 0)
        return;

    metrics_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = FAKEJOB_MAGIC;
    rec.version = FAKEJOB_VERSION;
    rec.type = FAKEJOB_TYPE_T0;
    rec.pid = (uint32_t)getpid();
    rec.iter = 0;
    rec.start_ns = (uint64_t)release_ns;
    rec.finish_ns = 0;

    ssize_t sent = sendto(metrics_fd, &rec, FAKEJOB_TOTAL_SIZE, MSG_NOSIGNAL,
                          (struct sockaddr *)&metrics_dst, metrics_dst_len);
    (void)sent;
}

/**
 * @brief Open CSV log file for append and emit headers if the file is empty.
 * @param s        Sink to initialize (output).
 * @param path     File path. If NULL, logging is disabled.
 * @param kind     Work kind selected (kept for header compatibility).
 * @param mode     Activation mode.
 * @param period   Task period (ms).
 * @param exec     CPU budget (ms).
 * @param sched_desc Descriptive scheduling string.
 * @return true on success (or disabled), false on fatal error opening file.
 */
static bool log_open(void **sink, const char *path,
                     activation_mode_t mode,
                     int64_t period, int64_t exec, const char *sched_desc)
{
    (void)sink;
    if (!path)
        return true; /* logging disabled */

    FILE *fp = fopen(path, "w");
    if (!fp)
    {
        fprintf(stderr, "Failed to open log file '%s': %s\n", path, strerror(errno));
        return false;
    }

    /* Detect empty file to print header once. */
    int need_header = 0;
    if (fseek(fp, 0, SEEK_END) == 0)
    {
        long pos = ftell(fp);
        if (pos == 0)
            need_header = 1;
    }

    if (need_header)
    {
        /* Task-level info header */
        if (mode == ACT_MODE_PERIODIC)
        {
            fprintf(fp, "# pid=%d, activation=%s, period_ms=%" PRId64 ", exec_ms=%" PRId64 ", sched=%s, work_kind=spin\n",
                    (int)getpid(), activation_mode_str(mode), period, exec,
                    (sched_desc ? sched_desc : "n/a"));
        }
        else
        {
            fprintf(fp, "# pid=%d, activation=%s, exec_ms=%" PRId64 ", sched=%s, work_kind=spin\n",
                    (int)getpid(), activation_mode_str(mode), exec,
                    (sched_desc ? sched_desc : "n/a"));
        }
        /* Column header */
        fprintf(fp,
                "iter,release_ms,start_time_ms,finish_time_ms,response_time_ms,execution_time_ms,lateness_ms,deadline_miss\n");
        fflush(fp);
    }

    *sink = fp;
    return true;
}

static inline void log_write_row(void *sink,
                                 int64_t iter,
                                 int64_t release_ms,
                                 int64_t start_time_ms,
                                 int64_t finish_time_ms,
                                 int64_t response_time_ms,
                                 int64_t execution_time_ms,
                                 int64_t lateness_ms,
                                 bool deadline_miss)
{
    FILE *fp = (FILE *)sink;
    if (fp == NULL)
        return;
    fprintf(fp,
            "%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%s\n",
            iter,
            release_ms,
            start_time_ms,
            finish_time_ms,
            response_time_ms,
            execution_time_ms,
            lateness_ms,
            deadline_miss ? "yes" : "no");
    fflush(fp);
}

static inline void log_close(void *sink)
{
    if (sink != NULL)
    {
        fclose((FILE *)sink);
    }
}

/*** Work budget **********************************************************/

/**
 * @brief Consume a given CPU-time budget (in nanoseconds) with a spin work step.
 *
 * The loop runs until the thread CPU time (this thread) has advanced by budget_ns.
 * Overshoot is bounded by the duration of a single iteration.
 *
 * @param budget_ns Desired CPU time to consume, in nanoseconds.
 */
static void fake_work_cpu_ns(int64_t budget_ns)
{
    /* Stop a little before the nominal budget to reduce systematic overshoot from loop/step latency. */
    const int64_t slack_ns = 100000; /* 100 µs */
    int64_t now = thread_cpu_time_ns();
    int64_t target = now + budget_ns;
    if (budget_ns > slack_ns)
        target -= slack_ns;
    for (;;)
    {
        if (thread_cpu_time_ns() >= target)
            break;
        __asm__ __volatile__("" ::: "memory");
    }
}

/*** Periodic loop *********************************************************/

/**
 * @brief Run a periodic task with CPU-time budget and absolute scheduling.
 *
 * Each iteration:
 *  - Waits until the next absolute release time (based on period).
 *  - Consumes exec_ms of CPU time, or a randomized budget when requested.
 *  - Emits one FAKEJOB record (pid, iter, start_ns, finish_ns) per job to the metrics socket.
 *  - Reports deadline miss if wall-clock completion is after the next release.
 *  - Optionally logs per-iteration metrics to a CSV sink.
 *
 * @param period_ms  Period of the task in milliseconds (T > 0).
 * @param exec_ms    WCET per job in milliseconds (C >= 0).
 * @param rand_exec  Randomize each job budget according to the configured Beta distribution.
 * @param iterations Number of iterations to run; if negative, runs forever.
 * @param sink       Optional CSV sink (FILE*, or NULL to disable logging).
 */
static void run_periodic_task(int64_t period_ms, int64_t exec_ms, int64_t iterations,
                              bool rand_exec, void *sink)
{
    const int64_t period_ns = NS_FROM_MS(period_ms);
    int64_t next_release = now_monotonic_ns(); // start immediately
    const int64_t first_release = next_release;
    metrics_emit_t0(first_release);

    int64_t k = 0;
    while (iterations < 0 || k < iterations)
    {
        sleep_until_abs_monotonic_ns(next_release);

        const int64_t release_ns = next_release;
        const int64_t next_release_ns = release_ns + period_ns;
        const int64_t job_exec_ns = select_exec_budget_ns(exec_ms, rand_exec);
        const int64_t start_time = now_monotonic_ns();
        const int64_t start_cpu = thread_cpu_time_ns();

        fake_work_cpu_ns(job_exec_ns);

        const int64_t finish_time = now_monotonic_ns();
        const int64_t end_cpu = thread_cpu_time_ns();

        /* Repeating the same origin lets a collector attach after this task
         * has started and synchronize before the following job record. */
        metrics_emit_t0(first_release);
        metrics_emit_job(k, start_time, finish_time);

        const int64_t release_ms = release_ns / 1000000LL;
        const int64_t start_time_ms = start_time / 1000000LL;
        const int64_t finish_time_ms = finish_time / 1000000LL;

        const int64_t response_time_ms = (finish_time - release_ns) / 1000000LL;
        const int64_t execution_time_ms = (end_cpu - start_cpu) / 1000000LL;

         next_release = next_release_ns;

         const int64_t lateness_ms = (finish_time - next_release) / 1000000LL;
         const bool deadline_miss = (lateness_ms > 0) ? true : false;

         if (g_verbose)
        {
            fprintf(stdout,
                    "%s | response_time=%" PRId64 " ms | execution_time=%" PRId64
                    " ms | target_exec=%.3f ms | lateness=%" PRId64 " ms\n",
                    deadline_miss ? "KO" : "OK",
                    response_time_ms, execution_time_ms,
                    (double)job_exec_ns / 1000000.0, lateness_ms);
        }

        log_write_row(sink, k, release_ms, start_time_ms,
                      finish_time_ms, response_time_ms,
                      execution_time_ms, lateness_ms, deadline_miss);

        ++k;
    }
}

/**
 * @brief Run a continuous task whose jobs are chained back-to-back.
 *
 * Each iteration:
 *  - Releases a job immediately after the previous one completes.
 *  - Consumes exec_ms of CPU time, or a randomized budget when requested.
 *  - Emits one FAKEJOB record (pid, iter, start_ns, finish_ns) per job to the metrics socket.
 *
 * @param exec_ms    WCET per job in milliseconds (C >= 0).
 * @param rand_exec  Randomize each job budget according to the configured Beta distribution.
 * @param iterations Number of jobs to run; if negative, runs forever.
 * @param sink       Optional CSV sink (FILE*, or NULL to disable logging).
 */
static void run_continuous_task(int64_t exec_ms, int64_t iterations,
                                bool rand_exec, void *sink)
{
    int64_t next_release = now_monotonic_ns(); /* first job released immediately */
    metrics_emit_t0(next_release);

    int64_t k = 0;
    while (iterations < 0 || k < iterations)
    {
        const int64_t release_ns = next_release;
        const int64_t job_exec_ns = select_exec_budget_ns(exec_ms, rand_exec);
        const int64_t start_time = now_monotonic_ns();
        const int64_t start_cpu = thread_cpu_time_ns();

        fake_work_cpu_ns(job_exec_ns);

        const int64_t finish_time = now_monotonic_ns();
        const int64_t end_cpu = thread_cpu_time_ns();

        metrics_emit_job(k, start_time, finish_time);

        const int64_t release_ms = release_ns / 1000000LL;
        const int64_t start_time_ms = start_time / 1000000LL;
        const int64_t finish_time_ms = finish_time / 1000000LL;
        const int64_t response_time_ms = (finish_time - release_ns) / 1000000LL;
        const int64_t execution_time_ms = (end_cpu - start_cpu) / 1000000LL;

        next_release = finish_time;

        if (g_verbose)
        {
            fprintf(stdout,
                    "OK | response_time=%" PRId64 " ms | execution_time=%" PRId64
                    " ms | target_exec=%.3f ms | activation=continuous\n",
                    response_time_ms, execution_time_ms,
                    (double)job_exec_ns / 1000000.0);
        }

        log_write_row(sink, k, release_ms, start_time_ms,
                      finish_time_ms, response_time_ms,
                      execution_time_ms, 0, false);

        ++k;
    }
}

/* -------------------------------------------------------------------------- */
/* Start gate using signals                                                    */
/* -------------------------------------------------------------------------- */

static bool g_wait_start = false;   /* enabled by -w */
static int g_wait_signal = SIGUSR1;

/**
 * @brief Wait for a synchronization signal before starting the activation loop.
 * @return true when the expected signal was successfully received and the
 *         start gate is released; false if setup or waiting failed.
 */
static bool wait_for_start_signal(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, g_wait_signal);

    /* Block the signal so we can synchronously wait for it. */
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
    {
        perror("pthread_sigmask");
        return false;
    }

    if (g_verbose)
    {
        fprintf(stdout, "Waiting for signal %d (SIGUSR1) to start… [pid=%ld]\n",
                g_wait_signal, (long)getpid());
        fflush(stdout);
    }

    int sig = 0;
    for (;;)
    {
        int rc = sigwait(&set, &sig);
        if (rc == 0 && sig == g_wait_signal)
            break; /* released */
        if (rc == EINTR)
            continue; /* spurious */
        fprintf(stderr, "sigwait failed: %s\n", strerror(rc));
        return false;
    }

    if (g_verbose)
    {
        fprintf(stdout, "Start gate released by signal %d.\n", sig);
    }
    return true;
}

/*** CLI parsing ***************************************************************/

typedef enum
{
    SCH_POLICY_FAIR = 0,
    SCH_POLICY_FIFO,
    SCH_POLICY_RR,
    SCH_POLICY_DL
} sched_policy_t;

typedef struct
{
    sched_policy_t policy;
    bool policy_set;
    bool nice_set;
    int nice_value;
    bool rt_prio_set;
    int rt_prio;
    int rt_prio_effective;
    bool dl_runtime_set;
    bool dl_period_set;
    bool dl_deadline_set;
    int64_t dl_runtime_ns;
    int64_t dl_period_ns;
    int64_t dl_deadline_ns;
} sched_config_t;

static const char *sched_policy_str(sched_policy_t p)
{
    switch (p)
    {
    case SCH_POLICY_FAIR:
        return "fair";
    case SCH_POLICY_FIFO:
        return "fifo";
    case SCH_POLICY_RR:
        return "rr";
    case SCH_POLICY_DL:
        return "dl";
    default:
        return "unknown";
    }
}

static bool parse_sched_policy(const char *s, sched_policy_t *out)
{
    if (!s || !out)
        return false;

    if (strcasecmp(s, "fair") == 0 || strcasecmp(s, "other") == 0)
    {
        *out = SCH_POLICY_FAIR;
        return true;
    }
    if (strcasecmp(s, "fifo") == 0)
    {
        *out = SCH_POLICY_FIFO;
        return true;
    }
    if (strcasecmp(s, "rr") == 0)
    {
        *out = SCH_POLICY_RR;
        return true;
    }
    if (strcasecmp(s, "dl") == 0 || strcasecmp(s, "deadline") == 0)
    {
        *out = SCH_POLICY_DL;
        return true;
    }
    return false;
}

static bool apply_posix_rt_policy(int policy, int prio)
{
    int pmin = sched_get_priority_min(policy);
    int pmax = sched_get_priority_max(policy);
    if (pmin == -1 || pmax == -1)
    {
        return false;
    }
    if (prio < pmin || prio > pmax)
    {
        errno = EINVAL;
        return false;
    }

    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;

    if (sched_setscheduler(0, policy, &sp) != 0)
    {
        return false;
    }
    return true;
}

static int sched_setattr_wrap(pid_t pid, const struct sched_attr *attr, unsigned int flags)
{
    return syscall(SYS_sched_setattr, pid, attr, flags);
}

static bool apply_sched_config(const sched_config_t *cfg)
{
    if (!cfg)
        return false;

    switch (cfg->policy)
    {
    case SCH_POLICY_FAIR:
        if (cfg->nice_set)
        {
            if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) != 0)
            {
                return false;
            }
        }
        return true;
    case SCH_POLICY_FIFO:
        return apply_posix_rt_policy(SCHED_FIFO, cfg->rt_prio_effective);
    case SCH_POLICY_RR:
        return apply_posix_rt_policy(SCHED_RR, cfg->rt_prio_effective);
    case SCH_POLICY_DL:
    {
        struct sched_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.size = sizeof(attr);
        attr.sched_policy = SCHED_DEADLINE;
        attr.sched_runtime = (unsigned long long)cfg->dl_runtime_ns;
        attr.sched_deadline = (unsigned long long)cfg->dl_deadline_ns;
        attr.sched_period = (unsigned long long)cfg->dl_period_ns;

        if (sched_setattr_wrap(0, &attr, 0) != 0)
        {
            return false;
        }
        return true;
    }
    default:
        errno = EINVAL;
        return false;
    }
}

static void describe_sched_config(const sched_config_t *cfg, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0)
        return;
    buf[0] = '\0';

    if (!cfg)
    {
        snprintf(buf, buf_len, "unknown");
        return;
    }

    switch (cfg->policy)
    {
    case SCH_POLICY_FAIR:
        if (cfg->nice_set)
            snprintf(buf, buf_len, "fair(nice=%d)", cfg->nice_value);
        else
            snprintf(buf, buf_len, "fair");
        break;
    case SCH_POLICY_FIFO:
        snprintf(buf, buf_len, "fifo(prio=%d)", cfg->rt_prio_effective);
        break;
    case SCH_POLICY_RR:
        snprintf(buf, buf_len, "rr(prio=%d)", cfg->rt_prio_effective);
        break;
    case SCH_POLICY_DL:
        snprintf(buf, buf_len,
                 "deadline(runtime=%" PRId64 "ms, deadline=%" PRId64 "ms, period=%" PRId64 "ms)",
                 (int64_t)(cfg->dl_runtime_ns / 1000000LL),
                 (int64_t)(cfg->dl_deadline_ns / 1000000LL),
                 (int64_t)(cfg->dl_period_ns / 1000000LL));
        break;
    default:
        snprintf(buf, buf_len, "unknown");
        break;
    }
}

static bool parse_i64(const char *s, int64_t *out_value)
{
    if (!s || !*s)
        return false;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return false;
    *out_value = (int64_t)v;
    return true;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-v] [-w] [--periodic|--continuous|--mode=MODE] [-s POLICY] [-n NICE] [-p PRIO|--prio=PRIO]\n"
            "          [--dl-runtime=MS --dl-period=MS [--dl-deadline=MS]]\n"
            "          [--rand-exec] [--log=FILE] [--metrics-socket=PATH]\n"
            "          <period_ms> <exec_ms> [iterations]\n"
            "       %s [-v] [-w] [--continuous|--mode=continuous] [-s POLICY] [-n NICE] [-p PRIO|--prio=PRIO]\n"
            "          [--dl-runtime=MS --dl-period=MS --dl-deadline=MS]\n"
            "          [--rand-exec] [--log=FILE] [--metrics-socket=PATH]\n"
            "          <exec_ms> [iterations]\n"
            "\n"
            "Options:\n"
            "  -v                 : Verbose logging (print OK/KO for each iteration)\n"
            "  -w                 : Wait for SIGUSR1 before starting the activation loop\n"
            "  --periodic         : Use periodic activation mode (default)\n"
            "  --continuous       : Use continuous activation mode (jobs chained without idle time)\n"
            "  --mode=MODE        : Activation mode: periodic or continuous\n"
            "  -s POLICY          : Scheduling policy: fair (default), fifo, rr, dl\n"
            "  --sched=POLICY     : Long form for -s\n"
            "  -n NICE            : Niceness (for fair policy)\n"
            "  -p PRIO/--prio=PRIO: RT priority for fifo/rr (implies fifo if no -s given, requires CAP_SYS_NICE/root)\n"
            "  --dl-runtime=MS    : (dl) Runtime/budget in milliseconds (default=exec_ms)\n"
            "  --dl-period=MS     : (dl) Period in milliseconds (default=period_ms in periodic mode)\n"
            "  --dl-deadline=MS   : (dl) Relative deadline in milliseconds (default=dl-period)\n"
            "  --rand-exec        : Randomize each job execution time between WCET/2 and WCET\n"
            "                       with X ~ Beta(a,b); parameters are compile-time defines\n"
            "  --log=FILE         : Append per-iteration metrics to FILE as CSV\n"
            "  --metrics-socket=PATH:\n"
             "                       : Emit one FAKEJOB record per job over a Unix datagram\n"
            "                       : socket at PATH (abstract name if it starts with '@').\n"
            "                       : Several fake-task instances may share the same socket.\n"
            "                       : Metrics are disabled when this option is omitted.\n",
            prog, prog);
}

/**
 * @brief Entry point.
 */
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *log_path = NULL; /* optional CSV path */
    const char *metrics_socket = NULL;
    void *csv_sink = NULL;
    sched_config_t sched_cfg = {0};
    activation_mode_t activation_mode = ACT_MODE_PERIODIC;
    bool activation_mode_set = false;
    bool rand_exec = false;
    sched_cfg.policy = SCH_POLICY_FAIR;

    // Options parsing
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-')
    {
        if (strcmp(argv[argi], "-v") == 0)
        {
            g_verbose = true;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "-w") == 0)
        {
            g_wait_start = true;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "--periodic") == 0)
        {
            activation_mode = ACT_MODE_PERIODIC;
            activation_mode_set = true;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "--continuous") == 0)
        {
            activation_mode = ACT_MODE_CONTINUOUS;
            activation_mode_set = true;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "--rand-exec") == 0)
        {
            rand_exec = true;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "--mode=", 7) == 0)
        {
            const char *p = argv[argi] + 7;
            if (strcasecmp(p, "periodic") == 0)
            {
                activation_mode = ACT_MODE_PERIODIC;
            }
            else if (strcasecmp(p, "continuous") == 0 || strcasecmp(p, "continue") == 0)
            {
                activation_mode = ACT_MODE_CONTINUOUS;
            }
            else
            {
                fprintf(stderr, "Invalid --mode value: %s\n", p);
                return EXIT_FAILURE;
            }
            activation_mode_set = true;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "-s", 2) == 0)
        {
            const char *p = NULL;
            if (argv[argi][2] != '\0')
            {
                /* attached form: -sfifo */
                p = &argv[argi][2];
            }
            else
            {
                /* separate form: -s fifo */
                if (argi + 1 >= argc)
                {
                    fprintf(stderr, "Error: -s requires a policy argument\n");
                    print_usage(argv[0]);
                    return EXIT_FAILURE;
                }
                p = argv[argi + 1];
                ++argi;
            }

            if (!parse_sched_policy(p, &sched_cfg.policy))
            {
                fprintf(stderr, "Invalid policy for -s: %s\n", p);
                return EXIT_FAILURE;
            }
            sched_cfg.policy_set = true;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "--sched=", 8) == 0)
        {
            const char *p = argv[argi] + 8;
            if (!parse_sched_policy(p, &sched_cfg.policy))
            {
                fprintf(stderr, "Invalid --sched value: %s\n", p);
                return EXIT_FAILURE;
            }
            sched_cfg.policy_set = true;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "-n", 2) == 0)
        {
            const char *p = NULL;
            if (argv[argi][2] != '\0')
            {
                /* attached form: -n5 */
                p = &argv[argi][2];
            }
            else
            {
                if (argi + 1 >= argc)
                {
                    fprintf(stderr, "Error: -n requires an argument\n");
                    print_usage(argv[0]);
                    return EXIT_FAILURE;
                }
                p = argv[argi + 1];
                ++argi;
            }

            int64_t v = 0;
            if (!parse_i64(p, &v) || v < INT_MIN || v > INT_MAX)
            {
                fprintf(stderr, "Invalid nice value: %s\n", p);
                return EXIT_FAILURE;
            }
            sched_cfg.nice_value = (int)v;
            sched_cfg.nice_set = true;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "--nice=", 7) == 0)
        {
            const char *p = argv[argi] + 7;
            int64_t v = 0;
            if (!parse_i64(p, &v) || v < INT_MIN || v > INT_MAX)
            {
                fprintf(stderr, "Invalid --nice value: %s\n", p);
                return EXIT_FAILURE;
            }
            sched_cfg.nice_value = (int)v;
            sched_cfg.nice_set = true;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "-p", 2) == 0)
        {
            const char *p = NULL;
            if (argv[argi][2] != '\0')
            {
                /* attached form: -p50 */
                p = &argv[argi][2];
            }
            else
            {
                /* separate form: -p 50 */
                if (argi + 1 >= argc)
                {
                    fprintf(stderr, "Error: -p requires an argument\n");
                    print_usage(argv[0]);
                    return EXIT_FAILURE;
                }
                p = argv[argi + 1];
                ++argi;
            }

            int64_t v = 0;
            if (!parse_i64(p, &v) || v < INT_MIN || v > INT_MAX)
            {
                fprintf(stderr, "Invalid priority for -p: %s\n", p);
                return EXIT_FAILURE;
            }
            sched_cfg.rt_prio = (int)v;
            sched_cfg.rt_prio_set = true;
            if (!sched_cfg.policy_set)
            {
                sched_cfg.policy = SCH_POLICY_FIFO; /* backward compatibility with old -p */
                sched_cfg.policy_set = true;
            }
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "--prio=", 7) == 0)
        {
            const char *p = argv[argi] + 7;
            int64_t v = 0;
            if (!parse_i64(p, &v) || v < INT_MIN || v > INT_MAX)
            {
                fprintf(stderr, "Invalid --prio value: %s\n", p);
                return EXIT_FAILURE;
            }
            sched_cfg.rt_prio = (int)v;
            sched_cfg.rt_prio_set = true;
            if (!sched_cfg.policy_set)
            {
                sched_cfg.policy = SCH_POLICY_FIFO;
                sched_cfg.policy_set = true;
            }
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "--dl-runtime=", 13) == 0)
        {
            const char *p = argv[argi] + 13;
            int64_t v = 0;
            if (!parse_i64(p, &v) || v <= 0)
            {
                fprintf(stderr, "Invalid --dl-runtime value: %s\n", p);
                return EXIT_FAILURE;
            }
            if (sched_cfg.policy_set && sched_cfg.policy != SCH_POLICY_DL)
            {
                fprintf(stderr, "--dl-runtime requires policy dl\n");
                return EXIT_FAILURE;
            }
            sched_cfg.policy = SCH_POLICY_DL;
            sched_cfg.policy_set = true;
            sched_cfg.dl_runtime_ns = NS_FROM_MS(v);
            sched_cfg.dl_runtime_set = true;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "--dl-period=", 12) == 0)
        {
            const char *p = argv[argi] + 12;
            int64_t v = 0;
            if (!parse_i64(p, &v) || v <= 0)
            {
                fprintf(stderr, "Invalid --dl-period value: %s\n", p);
                return EXIT_FAILURE;
            }
            if (sched_cfg.policy_set && sched_cfg.policy != SCH_POLICY_DL)
            {
                fprintf(stderr, "--dl-period requires policy dl\n");
                return EXIT_FAILURE;
            }
            sched_cfg.policy = SCH_POLICY_DL;
            sched_cfg.policy_set = true;
            sched_cfg.dl_period_ns = NS_FROM_MS(v);
            sched_cfg.dl_period_set = true;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "--dl-deadline=", 14) == 0)
        {
            const char *p = argv[argi] + 14;
            int64_t v = 0;
            if (!parse_i64(p, &v) || v <= 0)
            {
                fprintf(stderr, "Invalid --dl-deadline value: %s\n", p);
                return EXIT_FAILURE;
            }
            if (sched_cfg.policy_set && sched_cfg.policy != SCH_POLICY_DL)
            {
                fprintf(stderr, "--dl-deadline requires policy dl\n");
                return EXIT_FAILURE;
            }
            sched_cfg.policy = SCH_POLICY_DL;
            sched_cfg.policy_set = true;
            sched_cfg.dl_deadline_ns = NS_FROM_MS(v);
            sched_cfg.dl_deadline_set = true;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "--log=", 6) == 0)
        {
            if (strlen(argv[argi]) <= 6)
            {
                fprintf(stderr, "Invalid --log value\n");
                return EXIT_FAILURE;
            }
            log_path = argv[argi] + 6;
            ++argi;
            continue;
        }
        if (strncmp(argv[argi], "--metrics-socket=", 17) == 0)
        {
            const char *p = argv[argi] + 17;
            if (*p == '\0')
            {
                fprintf(stderr, "Invalid --metrics-socket value\n");
                return EXIT_FAILURE;
            }
            metrics_socket = p;
            ++argi;
            continue;
        }
        // Unknown option
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!activation_mode_set)
    {
        fprintf(stderr, "Activation mode is required: use --periodic or --continuous.\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    int64_t period_ms = -1;
    int64_t exec_ms = -1;
    int64_t iterations = -1;

    if (activation_mode == ACT_MODE_PERIODIC)
    {
        if (argc - argi < 2 || argc - argi > 3)
        {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (!parse_i64(argv[argi], &period_ms) || period_ms <= 0)
        {
            fprintf(stderr, "Invalid period_ms: %s\n", argv[argi]);
            return EXIT_FAILURE;
        }
        if (!parse_i64(argv[argi + 1], &exec_ms) || exec_ms < 0)
        {
            fprintf(stderr, "Invalid exec_ms: %s\n", argv[argi + 1]);
            return EXIT_FAILURE;
        }
        if (argc - argi == 3 && !parse_i64(argv[argi + 2], &iterations))
        {
            fprintf(stderr, "Invalid iterations: %s\n", argv[argi + 2]);
            return EXIT_FAILURE;
        }
    }
    else
    {
        if (argc - argi < 1 || argc - argi > 2)
        {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (!parse_i64(argv[argi], &exec_ms) || exec_ms < 0)
        {
            fprintf(stderr, "Invalid exec_ms: %s\n", argv[argi]);
            return EXIT_FAILURE;
        }
        if (argc - argi == 2 && !parse_i64(argv[argi + 1], &iterations))
        {
            fprintf(stderr, "Invalid iterations: %s\n", argv[argi + 1]);
            return EXIT_FAILURE;
        }
    }

    if (rand_exec &&
        (!(RAND_EXEC_BETA_A > 0.0) ||
         !(RAND_EXEC_BETA_B > 0.0) ||
         !(RAND_EXEC_MIN_FACTOR >= 0.0) ||
         !(RAND_EXEC_MAX_FACTOR >= RAND_EXEC_MIN_FACTOR)))
    {
        fprintf(stderr,
                "Invalid random execution-time configuration: require a > 0, b > 0, "
                "and 0 <= min <= max.\n");
        return EXIT_FAILURE;
    }

    /* Finalize scheduling configuration */
    if (sched_cfg.policy == SCH_POLICY_DL)
    {
        if (!sched_cfg.dl_runtime_set)
        {
            if (exec_ms <= 0)
            {
                fprintf(stderr, "SCHED_DEADLINE default runtime would be zero; specify --dl-runtime.\n");
                return EXIT_FAILURE;
            }
            sched_cfg.dl_runtime_ns = NS_FROM_MS(exec_ms + 1); /* default to exec_ms + 1 ms */
            sched_cfg.dl_runtime_set = true;
        }
        if (!sched_cfg.dl_period_set)
        {
            if (activation_mode != ACT_MODE_PERIODIC)
            {
                fprintf(stderr, "Continuous mode with policy dl requires explicit --dl-period.\n");
                return EXIT_FAILURE;
            }
            sched_cfg.dl_period_ns = NS_FROM_MS(period_ms); /* default to task period */
            sched_cfg.dl_period_set = true;
        }
        if (!sched_cfg.dl_deadline_set)
        {
            sched_cfg.dl_deadline_ns = sched_cfg.dl_period_ns; /* default deadline = period */
            sched_cfg.dl_deadline_set = true;
        }
        if (sched_cfg.dl_runtime_ns <= 0 || sched_cfg.dl_period_ns <= 0 || sched_cfg.dl_deadline_ns <= 0)
        {
            fprintf(stderr, "SCHED_DEADLINE parameters must be positive.\n");
            return EXIT_FAILURE;
        }
        if (sched_cfg.dl_runtime_ns > sched_cfg.dl_deadline_ns || sched_cfg.dl_deadline_ns > sched_cfg.dl_period_ns)
        {
            fprintf(stderr, "Require runtime <= deadline <= period for SCHED_DEADLINE.\n");
            return EXIT_FAILURE;
        }
    }
    else
    {
        if (sched_cfg.dl_runtime_set || sched_cfg.dl_period_set || sched_cfg.dl_deadline_set)
        {
            fprintf(stderr, "Deadline parameters are only valid with policy dl.\n");
            return EXIT_FAILURE;
        }
    }

    if (sched_cfg.nice_set && sched_cfg.policy != SCH_POLICY_FAIR)
    {
        fprintf(stderr, "Warning: ignoring niceness because policy is %s.\n",
                sched_policy_str(sched_cfg.policy));
        sched_cfg.nice_set = false;
    }

    if (sched_cfg.policy == SCH_POLICY_FIFO || sched_cfg.policy == SCH_POLICY_RR)
    {
        const int pol = (sched_cfg.policy == SCH_POLICY_FIFO) ? SCHED_FIFO : SCHED_RR;
        int pmin = sched_get_priority_min(pol);
        int pmax = sched_get_priority_max(pol);
        if (pmin == -1 || pmax == -1)
        {
            perror("sched_get_priority_min/max");
            return EXIT_FAILURE;
        }
        int chosen_prio = sched_cfg.rt_prio_set ? sched_cfg.rt_prio : pmin;
        if (chosen_prio < pmin || chosen_prio > pmax)
        {
            fprintf(stderr,
                    "Invalid RT priority %d for policy %s (valid range [%d..%d]).\n",
                    chosen_prio, sched_policy_str(sched_cfg.policy), pmin, pmax);
            return EXIT_FAILURE;
        }
        sched_cfg.rt_prio_effective = chosen_prio;
    }
    else
    {
        if (sched_cfg.rt_prio_set)
        {
            fprintf(stderr,
                    "Warning: RT priority ignored because policy is %s.\n",
                    sched_policy_str(sched_cfg.policy));
        }
        sched_cfg.rt_prio_effective = 0;
    }

    if (!apply_sched_config(&sched_cfg))
    {
        if (errno == EPERM)
        {
            fprintf(stderr,
                    "Failed to set scheduling policy %s: permission denied (need CAP_SYS_NICE or root).\n",
                    sched_policy_str(sched_cfg.policy));
        }
        else if (errno == ENOTSUP)
        {
            fprintf(stderr, "Failed to set scheduling policy %s: not supported by kernel/toolchain.\n",
                    sched_policy_str(sched_cfg.policy));
        }
        else
        {
            perror("Failed to apply scheduling policy");
        }
        return EXIT_FAILURE;
    }

    if (activation_mode == ACT_MODE_PERIODIC && exec_ms > period_ms)
    {
        fprintf(stderr,
                "Warning: exec_ms (%" PRId64 ") > period_ms (%" PRId64 ") — deadline impossible.\n",
                exec_ms, period_ms);
    }

    char sched_desc[128];
    describe_sched_config(&sched_cfg, sched_desc, sizeof(sched_desc));

    if (!log_open(&csv_sink, log_path, activation_mode, period_ms, exec_ms, sched_desc))
    {
        // Failed to open log file -> abort to avoid silent data loss.
        return EXIT_FAILURE;
    }

    /* Metrics are optional and enabled only with --metrics-socket. */
    if (metrics_socket && !metrics_socket_open(metrics_socket))
    {
        fprintf(stderr, "Warning: metrics socket disabled.\n");
        metrics_fd = -1;
    }

    /* Print recap before starting (with or without wait) */
    if (g_verbose)
    {
        if (activation_mode == ACT_MODE_PERIODIC)
        {
            fprintf(stdout,
                    "Task config: activation=%s, period=%" PRId64 " ms, exec=%" PRId64 " ms, "
                    "sched=%s, work_kind=spin, rand_exec=%s, metrics=socket:%s. ",
                    activation_mode_str(activation_mode),
                    period_ms,
                    exec_ms,
                    sched_desc,
                    rand_exec ? "yes" : "no",
                    metrics_socket ? metrics_socket : "disabled");
        }
        else
        {
            fprintf(stdout,
                    "Task config: activation=%s, exec=%" PRId64 " ms, "
                    "sched=%s, work_kind=spin, rand_exec=%s, metrics=socket:%s. ",
                    activation_mode_str(activation_mode),
                    exec_ms,
                    sched_desc,
                    rand_exec ? "yes" : "no",
                    metrics_socket ? metrics_socket : "disabled");
        }

        if (g_wait_start)
        {
            fprintf(stdout, "Waiting for SIGUSR1 to start (pid=%ld)...\n",
                    (long)getpid());
        }
        else
        {
            fprintf(stdout, "Starting immediately...\n");
        }
    }

    if (g_wait_start)
    {
        if (!wait_for_start_signal())
        {
            fprintf(stderr, "Failed while waiting for start signal.\n");
            log_close(csv_sink);
            return EXIT_FAILURE;
        }
    }

    if (metrics_socket && metrics_fd < 0)
    {
        fprintf(stderr, "Warning: metrics socket is disabled; no FAKEJOB records will be emitted.\n");
    }

    if (activation_mode == ACT_MODE_PERIODIC)
        run_periodic_task(period_ms, exec_ms, iterations, rand_exec, csv_sink);
    else
        run_continuous_task(exec_ms, iterations, rand_exec, csv_sink);

    if (metrics_fd >= 0)
        close(metrics_fd);

    log_close(csv_sink);
    return EXIT_SUCCESS;
}
