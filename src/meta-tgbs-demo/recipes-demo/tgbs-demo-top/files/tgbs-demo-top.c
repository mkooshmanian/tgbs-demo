/*
 * SPDX-License-Identifier: MIT
 *
 * tgbs-demo-top - a deliberately small top(1)-like monitor for TGBS.
 *
 * /run/tgbs selects cgroups owned by tgbsctl.  The cgroup v2 hierarchy and
 * /proc provide all measurements; the monitor never changes scheduler state.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_CPUS 256
#define MAX_DOMAINS 128
#define MAX_TASKS 2048
#define NAME_SIZE 256
#define CPU_LIST_SIZE 512

#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_DIM "\033[2m"
#define C_GREEN "\033[32m"
#define C_YELLOW "\033[33m"
#define C_RED "\033[31m"
#define C_CYAN "\033[36m"

struct cpu_sample {
	unsigned long long total;
	unsigned long long idle;
	bool valid;
};

struct task_sample {
	pid_t tid;
	unsigned long long starttime;
	unsigned long long ticks;
	unsigned long long runtime_ns;
	unsigned long long switches;
	char domain[NAME_SIZE];
	char comm[NAME_SIZE];
	char state;
	int processor;
	int policy;
	int priority;
	double cpu_percent;
	double cpu_ms;
	double switches_per_sec;
	bool runtime_ns_valid;
};

struct domain_sample {
	char name[NAME_SIZE];
	char state[16];
	char cpus[CPU_LIST_SIZE];
	unsigned long long usage_usec;
	unsigned long long runtime_usec;
	unsigned long long period_usec;
	unsigned int cpu_count;
	unsigned int task_count;
	double cpu_percent;
	double budget_percent;
};

struct snapshot {
	struct cpu_sample cpus[MAX_CPUS];
	struct domain_sample domains[MAX_DOMAINS];
	struct task_sample tasks[MAX_TASKS];
	unsigned int cpu_count;
	unsigned int domain_count;
	unsigned int task_count;
	struct timespec taken;
};

static volatile sig_atomic_t stop_requested;
static struct termios saved_termios;
static bool termios_saved;
static bool use_color = true;
static const char *cgroup_root = "/sys/fs/cgroup";
static const char *run_root = "/run/tgbs";

static void restore_terminal(void)
{
	if (termios_saved) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
		termios_saved = false;
	}
}

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static const char *color(const char *value)
{
	return use_color ? value : "";
}

static int path_printf(char *out, size_t out_size, const char *fmt, ...)
{
	va_list args;
	int length;

	va_start(args, fmt);
	length = vsnprintf(out, out_size, fmt, args);
	va_end(args);
	return length >= 0 && (size_t)length < out_size ? 0 : -1;
}

static int read_text(const char *path, char *out, size_t out_size)
{
	int fd;
	ssize_t length;

	if (out_size == 0)
		return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	do {
		length = read(fd, out, out_size - 1);
	} while (length < 0 && errno == EINTR);
	close(fd);
	if (length < 0)
		return -1;
	out[length] = '\0';
	while (length > 0 && isspace((unsigned char)out[length - 1]))
		out[--length] = '\0';
	return 0;
}

static int read_u64(const char *path, unsigned long long *value)
{
	char text[64];
	char *end;

	if (read_text(path, text, sizeof(text)) != 0)
		return -1;
	errno = 0;
	*value = strtoull(text, &end, 10);
	if (errno != 0 || end == text)
		return -1;
	while (isspace((unsigned char)*end))
		end++;
	return *end == '\0' ? 0 : -1;
}

static int read_keyed_u64(const char *path, const char *key,
		unsigned long long *value)
{
	FILE *stream;
	char line[256];
	char found_key[128];
	unsigned long long found_value;

	stream = fopen(path, "r");
	if (stream == NULL)
		return -1;
	while (fgets(line, sizeof(line), stream) != NULL) {
		if (sscanf(line, "%127s %llu", found_key, &found_value) == 2 &&
		    strcmp(found_key, key) == 0) {
			fclose(stream);
			*value = found_value;
			return 0;
		}
	}
	fclose(stream);
	return -1;
}

static bool valid_domain_name(const char *name)
{
	size_t length = strlen(name);

	if (length == 0 || length >= NAME_SIZE)
		return false;
	for (size_t i = 0; i < length; i++) {
		unsigned char c = (unsigned char)name[i];
		if (!isalnum(c) && c != '_' && c != '.' && c != '-')
			return false;
	}
	return true;
}

static unsigned int count_cpu_list(const char *list)
{
	const char *cursor = list;
	unsigned int count = 0;

	while (*cursor != '\0') {
		char *end;
		unsigned long first;
		unsigned long last;

		errno = 0;
		first = strtoul(cursor, &end, 10);
		if (errno != 0 || end == cursor)
			return 0;
		last = first;
		cursor = end;
		if (*cursor == '-') {
			cursor++;
			last = strtoul(cursor, &end, 10);
			if (errno != 0 || end == cursor || last < first)
				return 0;
			cursor = end;
		}
		if (last - first + 1 > UINT_MAX - count)
			return 0;
		count += (unsigned int)(last - first + 1);
		if (*cursor == '\0')
			break;
		if (*cursor++ != ',' || *cursor == '\0')
			return 0;
	}
	return count;
}

static int read_cpus(struct snapshot *snapshot)
{
	FILE *stream = fopen("/proc/stat", "r");
	char line[512];

	if (stream == NULL)
		return -1;
	while (fgets(line, sizeof(line), stream) != NULL) {
		unsigned int cpu;
		unsigned long long user, nice, system, idle, iowait;
		unsigned long long irq, softirq, steal;
		int fields;

		if (sscanf(line, "cpu%u ", &cpu) != 1)
			continue;
		if (cpu >= MAX_CPUS)
			continue;
		fields = sscanf(line, "cpu%*u %llu %llu %llu %llu %llu %llu %llu %llu",
			&user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
		if (fields < 4)
			continue;
		if (fields < 5)
			iowait = 0;
		if (fields < 6)
			irq = 0;
		if (fields < 7)
			softirq = 0;
		if (fields < 8)
			steal = 0;
		snapshot->cpus[cpu].total = user + nice + system + idle + iowait +
			irq + softirq + steal;
		snapshot->cpus[cpu].idle = idle + iowait;
		snapshot->cpus[cpu].valid = true;
		if (cpu + 1 > snapshot->cpu_count)
			snapshot->cpu_count = cpu + 1;
	}
	fclose(stream);
	return 0;
}

static int parse_task_stat(pid_t tid, struct task_sample *task)
{
	char path[PATH_MAX];
	char line[2048];
	char *close_paren;
	char *cursor;
	char *saveptr = NULL;
	char *token;
	unsigned long long utime = 0, stime = 0;
	int raw_priority = 0;

	/* Always use the task view. /proc/<leader>/stat is the process (TGID)
	 * view and includes its threads, which would double count them because
	 * cgroup.threads also returns every individual TID. */
	if (path_printf(path, sizeof(path), "/proc/%d/task/%d/stat",
		    (int)tid, (int)tid) != 0 ||
	    read_text(path, line, sizeof(line)) != 0)
		return -1;
	close_paren = strrchr(line, ')');
	if (close_paren == NULL)
		return -1;
	cursor = close_paren + 1;
	for (int field = 3; field <= 41; field++) {
		token = strtok_r(field == 3 ? cursor : NULL, " \t", &saveptr);
		if (token == NULL)
			return -1;
		switch (field) {
		case 3: task->state = token[0]; break;
		case 14: utime = strtoull(token, NULL, 10); break;
		case 15: stime = strtoull(token, NULL, 10); break;
		case 18: raw_priority = (int)strtol(token, NULL, 10); break;
		case 22: task->starttime = strtoull(token, NULL, 10); break;
		case 39: task->processor = (int)strtol(token, NULL, 10); break;
		case 41: task->policy = (int)strtol(token, NULL, 10); break;
		default: break;
		}
	}
	task->ticks = utime + stime;
	if (task->policy == SCHED_FIFO || task->policy == SCHED_RR) {
		struct sched_param param;
		if (sched_getparam(tid, &param) == 0)
			task->priority = param.sched_priority;
		else
			task->priority = raw_priority < 0 ? -raw_priority - 1 : 0;
	} else {
		task->priority = raw_priority - 20;
	}
	return 0;
}

static void sanitize(char *text)
{
	for (; *text != '\0'; text++) {
		if (!isprint((unsigned char)*text))
			*text = '?';
	}
}

static void read_task_switches(pid_t tid, unsigned long long *switches)
{
	char path[PATH_MAX];
	FILE *stream;
	char line[256];
	unsigned long long total = 0;

	if (path_printf(path, sizeof(path), "/proc/%d/task/%d/status",
		    (int)tid, (int)tid) != 0)
		return;
	stream = fopen(path, "r");
	if (stream == NULL)
		return;
	while (fgets(line, sizeof(line), stream) != NULL) {
		unsigned long long value;
		if (sscanf(line, "voluntary_ctxt_switches: %llu", &value) == 1 ||
		    sscanf(line, "nonvoluntary_ctxt_switches: %llu", &value) == 1)
			total += value;
	}
	fclose(stream);
	*switches = total;
}

/* The first /proc/<tid>/schedstat field is sum_exec_runtime in nanoseconds.
 * Unlike utime/stime it is not quantized to the system clock tick, which keeps
 * short periodic jobs visible.  Old/minimal kernels can omit the file; the
 * delta code then falls back to utime + stime. */
static void read_task_runtime(pid_t tid, struct task_sample *task)
{
	char path[PATH_MAX];
	FILE *stream;

	if (path_printf(path, sizeof(path), "/proc/%d/task/%d/schedstat",
		    (int)tid, (int)tid) != 0)
		return;
	stream = fopen(path, "r");
	if (stream == NULL)
		return;
	if (fscanf(stream, "%llu", &task->runtime_ns) == 1)
		task->runtime_ns_valid = true;
	fclose(stream);
}

static void read_task(struct snapshot *snapshot, struct domain_sample *domain,
		pid_t tid)
{
	struct task_sample *task;
	char path[PATH_MAX];

	if (snapshot->task_count >= MAX_TASKS)
		return;
	task = &snapshot->tasks[snapshot->task_count];
	memset(task, 0, sizeof(*task));
	task->tid = tid;
	task->processor = -1;
	if (parse_task_stat(tid, task) != 0)
		return;
	if (path_printf(path, sizeof(path), "/proc/%d/task/%d/comm",
		    (int)tid, (int)tid) == 0 &&
	    read_text(path, task->comm, sizeof(task->comm)) == 0)
		sanitize(task->comm);
	else
		strcpy(task->comm, "?");
	read_task_runtime(tid, task);
	read_task_switches(tid, &task->switches);
	strncpy(task->domain, domain->name, sizeof(task->domain) - 1);
	snapshot->task_count++;
	domain->task_count++;
}

static void read_domain_tasks(struct snapshot *snapshot,
		struct domain_sample *domain, const char *path)
{
	FILE *stream = fopen(path, "r");
	long tid;

	if (stream == NULL)
		return;
	while (fscanf(stream, "%ld", &tid) == 1) {
		if (tid > 0 && tid <= INT_MAX)
			read_task(snapshot, domain, (pid_t)tid);
	}
	fclose(stream);
}

static void read_domain_state(const char *path, char *state, size_t state_size)
{
	unsigned long long populated = 0;
	unsigned long long frozen = 0;

	if (read_keyed_u64(path, "populated", &populated) != 0) {
		strncpy(state, "unknown", state_size);
		return;
	}
	read_keyed_u64(path, "frozen", &frozen);
	strncpy(state, frozen ? "paused" : populated ? "running" : "empty",
		state_size);
}

static void read_domain(struct snapshot *snapshot, const char *name)
{
	struct domain_sample *domain;
	char path[PATH_MAX];
	struct stat status;

	if (snapshot->domain_count >= MAX_DOMAINS || !valid_domain_name(name))
		return;
	if (path_printf(path, sizeof(path), "%s/%s", cgroup_root, name) != 0 ||
	    stat(path, &status) != 0 || !S_ISDIR(status.st_mode))
		return;
	domain = &snapshot->domains[snapshot->domain_count];
	memset(domain, 0, sizeof(*domain));
	strncpy(domain->name, name, sizeof(domain->name) - 1);

	path_printf(path, sizeof(path), "%s/%s/cpu.stat", cgroup_root, name);
	read_keyed_u64(path, "usage_usec", &domain->usage_usec);
	path_printf(path, sizeof(path), "%s/%s/cpu.runtime_us", cgroup_root, name);
	read_u64(path, &domain->runtime_usec);
	path_printf(path, sizeof(path), "%s/%s/cpu.period_us", cgroup_root, name);
	read_u64(path, &domain->period_usec);
	path_printf(path, sizeof(path), "%s/%s/cpuset.cpus.effective", cgroup_root, name);
	if (read_text(path, domain->cpus, sizeof(domain->cpus)) != 0)
		strcpy(domain->cpus, "-");
	domain->cpu_count = count_cpu_list(domain->cpus);
	if (domain->period_usec != 0)
		domain->budget_percent = 100.0 * (double)domain->runtime_usec /
			(double)domain->period_usec;
	path_printf(path, sizeof(path), "%s/%s/cgroup.events", cgroup_root, name);
	read_domain_state(path, domain->state, sizeof(domain->state));
	path_printf(path, sizeof(path), "%s/%s/cgroup.threads", cgroup_root, name);
	read_domain_tasks(snapshot, domain, path);
	snapshot->domain_count++;
}

static void take_snapshot(struct snapshot *snapshot)
{
	DIR *directory;
	struct dirent *entry;

	memset(snapshot, 0, sizeof(*snapshot));
	clock_gettime(CLOCK_MONOTONIC, &snapshot->taken);
	read_cpus(snapshot);
	directory = opendir(run_root);
	if (directory == NULL)
		return;
	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] != '.')
			read_domain(snapshot, entry->d_name);
	}
	closedir(directory);
}

static const struct domain_sample *previous_domain(const struct snapshot *snapshot,
		const char *name)
{
	for (unsigned int i = 0; i < snapshot->domain_count; i++) {
		if (strcmp(snapshot->domains[i].name, name) == 0)
			return &snapshot->domains[i];
	}
	return NULL;
}

static const struct task_sample *previous_task(const struct snapshot *snapshot,
		const struct task_sample *task)
{
	for (unsigned int i = 0; i < snapshot->task_count; i++) {
		const struct task_sample *old = &snapshot->tasks[i];
		if (old->tid == task->tid && old->starttime == task->starttime)
			return old;
	}
	return NULL;
}

static double elapsed_seconds(const struct timespec *newer,
		const struct timespec *older)
{
	return (double)(newer->tv_sec - older->tv_sec) +
		(double)(newer->tv_nsec - older->tv_nsec) / 1000000000.0;
}

static void calculate_deltas(struct snapshot *current,
		const struct snapshot *previous)
{
	double elapsed = elapsed_seconds(&current->taken, &previous->taken);
	long ticks_per_second = sysconf(_SC_CLK_TCK);

	if (elapsed <= 0.0)
		return;
	for (unsigned int i = 0; i < current->domain_count; i++) {
		struct domain_sample *domain = &current->domains[i];
		const struct domain_sample *old = previous_domain(previous, domain->name);
		if (old != NULL && domain->usage_usec >= old->usage_usec)
			domain->cpu_percent = (double)(domain->usage_usec - old->usage_usec) /
				(elapsed * 10000.0);
	}
	if (ticks_per_second <= 0)
		return;
	for (unsigned int i = 0; i < current->task_count; i++) {
		struct task_sample *task = &current->tasks[i];
		const struct task_sample *old = previous_task(previous, task);
		unsigned long long delta_ticks;
		unsigned long long delta_ns;

		if (old == NULL)
			continue;
		if (task->runtime_ns_valid && old->runtime_ns_valid &&
		    task->runtime_ns >= old->runtime_ns) {
			delta_ns = task->runtime_ns - old->runtime_ns;
			task->cpu_percent = (double)delta_ns / (elapsed * 10000000.0);
			task->cpu_ms = (double)delta_ns / 1000000.0;
		} else if (task->ticks >= old->ticks) {
			delta_ticks = task->ticks - old->ticks;
			task->cpu_percent = 100.0 * (double)delta_ticks /
				((double)ticks_per_second * elapsed);
			task->cpu_ms = 1000.0 * (double)delta_ticks /
				(double)ticks_per_second;
		}
		if (task->switches >= old->switches)
			task->switches_per_sec = (double)(task->switches - old->switches) /
				elapsed;
	}
}

static int compare_domains(const void *left, const void *right)
{
	const struct domain_sample *a = left;
	const struct domain_sample *b = right;
	if (a->cpu_percent < b->cpu_percent)
		return 1;
	if (a->cpu_percent > b->cpu_percent)
		return -1;
	return strcmp(a->name, b->name);
}

static int compare_tasks(const void *left, const void *right)
{
	const struct task_sample *a = left;
	const struct task_sample *b = right;
	int domain_order = strcmp(a->domain, b->domain);
	if (domain_order != 0)
		return domain_order;
	if (a->cpu_percent < b->cpu_percent)
		return 1;
	if (a->cpu_percent > b->cpu_percent)
		return -1;
	return a->tid > b->tid ? 1 : a->tid < b->tid ? -1 : 0;
}

static const char *policy_name(int policy)
{
	switch (policy) {
	case SCHED_OTHER: return "OTHER";
	case SCHED_FIFO: return "FIFO";
	case SCHED_RR: return "RR";
#ifdef SCHED_BATCH
	case SCHED_BATCH: return "BATCH";
#endif
#ifdef SCHED_IDLE
	case SCHED_IDLE: return "IDLE";
#endif
#ifdef SCHED_DEADLINE
	case SCHED_DEADLINE: return "DEADLINE";
#endif
	default: return "?";
	}
}

static void terminal_size(unsigned int *rows, unsigned int *columns)
{
	struct winsize size;
	*rows = 24;
	*columns = 100;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
		if (size.ws_row > 0)
			*rows = size.ws_row;
		if (size.ws_col > 0)
			*columns = size.ws_col;
	}
}

static void print_bar(double percent, unsigned int width, const char *bar_color)
{
	unsigned int filled;

	if (percent < 0.0)
		percent = 0.0;
	if (percent > 100.0)
		percent = 100.0;
	filled = (unsigned int)(percent * width / 100.0 + 0.5);
	printf("%s[", color(bar_color));
	for (unsigned int i = 0; i < width; i++)
		putchar(i < filled ? '#' : '-');
	printf("]%s", color(C_RESET));
}

static const char *load_color(double percent)
{
	if (percent >= 90.0)
		return C_RED;
	if (percent >= 70.0)
		return C_YELLOW;
	return C_GREEN;
}

static double cpu_load(const struct cpu_sample *current,
		const struct cpu_sample *previous)
{
	unsigned long long total_delta;
	unsigned long long idle_delta;

	if (!current->valid || !previous->valid || current->total < previous->total ||
	    current->idle < previous->idle)
		return 0.0;
	total_delta = current->total - previous->total;
	idle_delta = current->idle - previous->idle;
	if (total_delta == 0 || idle_delta > total_delta)
		return 0.0;
	return 100.0 * (double)(total_delta - idle_delta) / (double)total_delta;
}

static void render(const struct snapshot *current, const struct snapshot *previous,
		double interval, bool interactive)
{
	unsigned int rows, columns;
	unsigned int printed_rows = 0;
	unsigned int bar_width;
	double tgbs_percent = 0.0;
	double all_busy = 0.0;
	double other_percent;
	double idle_percent;
	time_t now = time(NULL);
	struct tm local;
	char timestamp[32];

	terminal_size(&rows, &columns);
	bar_width = columns >= 110 ? 24 : columns >= 80 ? 14 : 8;
	if (interactive)
		printf("\033[H\033[2J");
	localtime_r(&now, &local);
	strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &local);
	printf("%s%sTGBS TOP%s  %s  sample %.2fs",
		color(C_BOLD), color(C_CYAN), color(C_RESET), timestamp, interval);
	if (interactive)
		printf("  %sq quit%s", color(C_DIM), color(C_RESET));
	putchar('\n');
	printed_rows++;

	printf("%sCPU OCCUPATION%s\n", color(C_BOLD), color(C_RESET));
	printed_rows++;
	for (unsigned int i = 0; i < current->cpu_count; i++) {
		double load = cpu_load(&current->cpus[i], &previous->cpus[i]);
		all_busy += load;
		printf(" CPU%-3u ", i);
		print_bar(load, bar_width, load_color(load));
		printf(" %6.1f%%\n", load);
		printed_rows++;
	}
	for (unsigned int i = 0; i < current->domain_count; i++)
		tgbs_percent += current->domains[i].cpu_percent;
	if (current->cpu_count > 0) {
		other_percent = all_busy - tgbs_percent;
		if (other_percent < 0.0)
			other_percent = 0.0;
		idle_percent = 100.0 * current->cpu_count - all_busy;
		if (idle_percent < 0.0)
			idle_percent = 0.0;
		printf(" ALL    TGBS %5.1f%% (%4.2f CPU)  OTHER %5.1f%%  IDLE %5.1f%%",
			tgbs_percent / current->cpu_count, tgbs_percent / 100.0,
			other_percent / current->cpu_count,
			idle_percent / current->cpu_count);
		printf("  %s(normalized over %u CPUs)%s\n", color(C_DIM),
			current->cpu_count, color(C_RESET));
		printed_rows++;
	}

	printf("\n%sTGBS DOMAINS%s  %sCPU%%: 100%% = one CPU; USE/BUDGET: actual / reserved%s\n",
		color(C_BOLD), color(C_RESET), color(C_DIM), color(C_RESET));
	printf(" %-18s %-8s %7s %10s  %-*s %8s %8s\n",
		"DOMAIN", "STATE", "CPU%", "BUDGET/CPU",
		(int)bar_width + 2, "USE/BUDGET", "CPUS", "NB TASKS");
	printed_rows += 3;
	if (current->domain_count == 0) {
		printf(" %sNo active tgbsctl-managed domain.%s\n",
			color(C_DIM), color(C_RESET));
		printed_rows++;
	}
	for (unsigned int i = 0; i < current->domain_count; i++) {
		const struct domain_sample *domain = &current->domains[i];
		const char *state_color = strcmp(domain->state, "running") == 0 ? C_GREEN :
			strcmp(domain->state, "paused") == 0 ? C_YELLOW : C_DIM;
		double total_budget = domain->budget_percent * domain->cpu_count;
		double budget_use = total_budget > 0.0 ?
			100.0 * domain->cpu_percent / total_budget : 0.0;
		printf(" %-18.18s %s%-8s%s %6.1f%% %9.1f%%  ", domain->name,
			color(state_color), domain->state, color(C_RESET),
			domain->cpu_percent, domain->budget_percent);
		print_bar(budget_use, bar_width, load_color(budget_use));
		printf(" %8.8s %8u\n", domain->cpus, domain->task_count);
		printed_rows++;
	}

	printf("\n%sTGBS TASKS%s  %s(kernel threads/processes in those domains only)%s\n",
		color(C_BOLD), color(C_RESET), color(C_DIM), color(C_RESET));
	printf(" %-14s %6s %-20s %-8s %4s %4s %7s %9s %8s %2s\n",
		"DOMAIN", "TID", "TASK", "POLICY", "PRI", "CPU", "CPU%",
		"CPU ms", "CSW/s", "S");
	printed_rows += 3;
	if (current->task_count == 0) {
		printf(" %sNo task to display.%s\n", color(C_DIM), color(C_RESET));
		printed_rows++;
	}
	for (unsigned int i = 0; i < current->task_count; i++) {
		const struct task_sample *task = &current->tasks[i];
		if (interactive && printed_rows + 1 >= rows) {
			printf(" %s... %u more task(s); enlarge the terminal%s\n",
				color(C_DIM), current->task_count - i, color(C_RESET));
			break;
		}
		printf(" %-14.14s %6d %-20.20s %-8s %4d %4d %6.1f%% %7.1fms %8.1f %2c\n",
			task->domain, (int)task->tid, task->comm, policy_name(task->policy),
			task->priority, task->processor, task->cpu_percent, task->cpu_ms,
			task->switches_per_sec, task->state ? task->state : '?');
		printed_rows++;
	}
	fflush(stdout);
}

static int wait_interval(double seconds, bool interactive)
{
	int timeout_ms = (int)(seconds * 1000.0 + 0.5);
	struct pollfd input = { .fd = STDIN_FILENO, .events = POLLIN };
	int remaining = timeout_ms;

	while (remaining > 0 && !stop_requested) {
		struct timespec before, after;
		int wait_ms = remaining;
		int result;

		clock_gettime(CLOCK_MONOTONIC, &before);
		result = poll(interactive ? &input : NULL, interactive ? 1 : 0, wait_ms);
		clock_gettime(CLOCK_MONOTONIC, &after);
		if (result > 0 && (input.revents & POLLIN)) {
			char key;
			if (read(STDIN_FILENO, &key, 1) == 1 && (key == 'q' || key == 'Q'))
				return 1;
		}
		if (result < 0 && errno != EINTR)
			return -1;
		remaining -= (int)(elapsed_seconds(&after, &before) * 1000.0);
		if (remaining == timeout_ms)
			remaining--;
	}
	return stop_requested ? 1 : 0;
}

static int enable_interactive_terminal(void)
{
	struct termios raw;

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		return 0;
	if (tcgetattr(STDIN_FILENO, &saved_termios) != 0)
		return 0;
	raw = saved_termios;
	raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
		return 0;
	termios_saved = true;
	return 1;
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s [OPTIONS]\n"
		"\n"
		"Top-like CPU view restricted to tgbsctl-managed domains and tasks.\n"
		"\n"
		"  -d, --delay SECONDS  refresh period (default: 1)\n"
		"  -n, --iterations N    stop after N displayed samples\n"
		"  -b, --batch           do not clear the terminal or read keys\n"
		"      --no-color        disable ANSI colors\n"
		"  -h, --help            show this help\n"
		"\n"
		"In interactive mode, press q to quit. CPU%% follows top semantics: 100%%\n"
		"means one fully occupied logical CPU. BUDGET/CPU is runtime / period.\n",
		program);
}

static int parse_positive_double(const char *text, double *value)
{
	char *end;
	errno = 0;
	*value = strtod(text, &end);
	return errno == 0 && end != text && *end == '\0' && *value >= 0.05 &&
		*value <= 3600.0 ? 0 : -1;
}

static int parse_positive_uint(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
	    parsed > UINT_MAX)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "delay", required_argument, NULL, 'd' },
		{ "iterations", required_argument, NULL, 'n' },
		{ "batch", no_argument, NULL, 'b' },
		{ "no-color", no_argument, NULL, 1000 },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	struct snapshot previous;
	struct snapshot current;
	double delay = 1.0;
	unsigned int iterations = 0;
	unsigned int displayed = 0;
	bool batch = false;
	bool iterations_set = false;
	int option;
	int interactive;
	const char *override;

	while ((option = getopt_long(argc, argv, "d:n:bh", long_options, NULL)) != -1) {
		switch (option) {
		case 'd':
			if (parse_positive_double(optarg, &delay) != 0) {
				fprintf(stderr, "tgbs-demo-top: invalid delay: %s\n", optarg);
				return 2;
			}
			break;
		case 'n':
			if (parse_positive_uint(optarg, &iterations) != 0) {
				fprintf(stderr, "tgbs-demo-top: invalid iteration count: %s\n", optarg);
				return 2;
			}
			iterations_set = true;
			break;
		case 'b': batch = true; break;
		case 1000: use_color = false; break;
		case 'h': usage(stdout, argv[0]); return 0;
		default: usage(stderr, argv[0]); return 2;
		}
	}
	if (optind != argc) {
		usage(stderr, argv[0]);
		return 2;
	}

	override = getenv("TGBS_TOP_CGROUP_ROOT");
	if (override != NULL && override[0] == '/')
		cgroup_root = override;
	override = getenv("TGBS_TOP_RUN_ROOT");
	if (override != NULL && override[0] == '/')
		run_root = override;
	interactive = !batch && enable_interactive_terminal();
	if (!interactive) {
		batch = true;
		use_color = false;
		if (!iterations_set)
			iterations = 1;
	}
	atexit(restore_terminal);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);

	take_snapshot(&previous);
	while (!stop_requested && (iterations == 0 || displayed < iterations)) {
		int wait_result = wait_interval(delay, interactive);
		if (wait_result != 0)
			break;
		take_snapshot(&current);
		calculate_deltas(&current, &previous);
		qsort(current.domains, current.domain_count, sizeof(current.domains[0]),
			compare_domains);
		qsort(current.tasks, current.task_count, sizeof(current.tasks[0]),
			compare_tasks);
		render(&current, &previous,
			elapsed_seconds(&current.taken, &previous.taken), interactive);
		previous = current;
		displayed++;
	}
	if (interactive)
		printf("\033[0m\n");
	return 0;
}
