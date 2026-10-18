/*
 * tgbsctl list / inspect -- read-only observation of active TGBS domains.
 *
 * cgroupfs under CG_ROOT is the single source of truth for resource and task
 * state. Domain directories below /run/tgbs hold volatile main.pid markers.
 * The separate channels subtree is ignored here. Nothing mutates the system.
 */

#include "tgbsctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>

static int read_cgroup_event(const char *name, const char *key, int *out)
{
	char path[PATH_MAX];
	char line[128];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s/cgroup.events", CG_ROOT, name);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;

	while (fgets(line, sizeof(line), f) != NULL) {
		char event[64];
		int value;

		if (sscanf(line, "%63s %d", event, &value) == 2 &&
		    strcmp(event, key) == 0) {
			fclose(f);
			*out = value;
			return 0;
		}
	}
	fclose(f);
	errno = ENOENT;
	return -1;
}

static int read_procs(const char *name, char *out, size_t outsz)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "%s/%s/cgroup.procs", CG_ROOT, name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	out[0] = '\0';
	size_t used = 0;
	while (1) {
		char pidbuf[32];
		ssize_t n = read(fd, pidbuf, sizeof(pidbuf) - 1);
		if (n <= 0)
			break;
		pidbuf[n] = '\0';
		for (char *p = pidbuf; *p != '\0'; p++) {
			if (!isdigit((unsigned char) *p))
				continue;
			pid_t pid = (pid_t) strtoul(p, &p, 10);
			if (pid <= 0)
				continue;
			int len = snprintf(out + used, outsz - used, "%s%d",
				used > 0 ? " " : "", (int) pid);
			if (len < 0 || (size_t) len >= outsz - used)
				break;
			used += len;
		}
	}
	close(fd);
	return 0;
}

static char *read_command(pid_t pid)
{
	char path[PATH_MAX];
	int fd;
	size_t used = 0;
	size_t capacity = 256;
	char *cmdline;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int) pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	cmdline = malloc(capacity);
	if (cmdline == NULL) {
		close(fd);
		return NULL;
	}

	while (1) {
		ssize_t r;

		if (used == capacity) {
			size_t new_capacity = capacity * 2;
			char *larger;

			if (new_capacity <= capacity) {
				free(cmdline);
				close(fd);
				return NULL;
			}
			larger = realloc(cmdline, new_capacity);
			if (larger == NULL) {
				free(cmdline);
				close(fd);
				return NULL;
			}
			cmdline = larger;
			capacity = new_capacity;
		}

		r = read(fd, cmdline + used, capacity - used);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			free(cmdline);
			close(fd);
			return NULL;
		}
		if (r == 0)
			break;
		used += (size_t) r;
	}
	close(fd);
	if (used == 0) {
		free(cmdline);
		return NULL;
	}

	/* /proc/<pid>/cmdline stores argv entries separated by NUL bytes and
	 * normally ends with one. Join every entry for display; the shell quoting
	 * used to create argv is not retained by the kernel. */
	while (used > 0 && cmdline[used - 1] == '\0')
		used--;
	for (size_t i = 0; i < used; i++) {
		if (cmdline[i] == '\0')
			cmdline[i] = ' ';
	}
	cmdline[used] = '\0';
	return cmdline;
}

static int read_config(const char *name, struct runtime_config *runtime,
		unsigned long long *period)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/cpu.runtime_us", CG_ROOT, name);
	if (read_runtime(path, runtime) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/%s/cpu.period_us", CG_ROOT, name);
	if (read_u64(path, period) != 0 || *period == 0) {
		free_runtime(runtime);
		return -1;
	}
	return 0;
}

static int read_cpus(const char *name, char *effective, size_t effective_sz)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s/cpuset.cpus.effective", CG_ROOT, name);
	if (read_text(path, effective, effective_sz) != 0)
		return -1;
	return 0;
}

static int read_reclaim(const char *name, int *reclaim)
{
	char path[PATH_MAX];
	unsigned long long value;

	snprintf(path, sizeof(path), "%s/%s/cpu.reclaim", CG_ROOT, name);
	if (read_u64(path, &value) != 0 || value > 1)
		return -1;
	*reclaim = value == 1;
	return 0;
}

static int read_resource_file(const char *name, const char *filename,
		char *out, size_t outsz)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s/%s", CG_ROOT, name, filename);
	return read_text(path, out, outsz);
}

static int format_bytes(const char *text, char *out, size_t outsz)
{
	static const char *const units[] = {
		"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"
	};
	unsigned long long bytes;
	long double value;
	char *end;
	size_t unit = 0;
	int len;

	if (strcmp(text, "max") == 0) {
		len = snprintf(out, outsz, "max");
		return len >= 0 && (size_t)len < outsz ? 0 : -1;
	}

	errno = 0;
	bytes = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return -1;

	value = (long double)bytes;
	while (value >= 1024.0L && unit + 1 < sizeof(units) / sizeof(units[0])) {
		value /= 1024.0L;
		unit++;
	}

	if (unit == 0)
		len = snprintf(out, outsz, "%llu %s", bytes, units[unit]);
	else
		len = snprintf(out, outsz, "%.2Lf %s", value, units[unit]);
	return len >= 0 && (size_t)len < outsz ? 0 : -1;
}

int read_main_meta(const char *name, pid_t *pid,
		unsigned long long *starttime)
{
	char path[PATH_MAX];
	char buf[128];
	int fd;
	ssize_t r;

	snprintf(path, sizeof(path), "%s/%s/main.pid", RUN_ROOT, name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = '\0';

	if (sscanf(buf, "%d %llu", (int *)pid, starttime) != 2)
		return -1;
	if (*pid <= 0 || *starttime == 0)
		return -1;
	return 0;
}

/* A main process is alive when /proc/<pid> exists, its starttime matches the
 * recorded one (guards PID reuse), and it is still tracked by the cgroup. */
static int main_is_alive(const char *name, pid_t pid, unsigned long long starttime)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/proc/%d", (int) pid);
	if (access(path, F_OK) != 0)
		return 0;

	unsigned long long now_start;
	if (read_starttime(pid, &now_start) != 0)
		return 0;
	if (now_start != starttime)
		return 0;

	int populated = -1;
	read_cgroup_event(name, "populated", &populated);
	return populated == 1;
}

static enum domain_state domain_state(const char *name, pid_t pid,
		unsigned long long starttime)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
	if (access(path, F_OK) != 0)
		return STATE_STALE;

	int populated = 0;
	read_cgroup_event(name, "populated", &populated);

	if (main_is_alive(name, pid, starttime)) {
		int frozen = 0;
		read_cgroup_event(name, "frozen", &frozen);
		return frozen ? STATE_PAUSED : STATE_RUNNING;
	}
	return populated ? STATE_ORPHAN : STATE_EXITED;
}

const char *state_name(enum domain_state s)
{
	switch (s) {
	case STATE_UNKNOWN: return "unknown";
	case STATE_INITIALIZING: return "initializing";
	case STATE_RUNNING: return "running";
	case STATE_PAUSED: return "paused";
	case STATE_ORPHAN: return "orphan";
	case STATE_EXITED: return "exited";
	case STATE_STALE: return "stale";
	}
	return "?";
}

/* Keep a scalar for uniform effective runtimes; otherwise identify each CPU.
 * The default runtime is displayed separately. */
static char *format_effective_runtime(const struct runtime_config *runtime,
		const struct cpu_list *effective)
{
	size_t size = 32 + effective->count * 40;
	char *text = malloc(size);
	unsigned long long first;
	int uniform = 1;
	size_t used = 0;

	if (text == NULL)
		return NULL;
	if (effective->count == 0) {
		strcpy(text, "-");
		return text;
	}
	first = runtime_for_cpu(runtime, effective->cpus[0]);
	for (size_t i = 1; i < effective->count; i++)
		if (runtime_for_cpu(runtime, effective->cpus[i]) != first) {
			uniform = 0;
			break;
		}
	if (uniform) {
		snprintf(text, size, "%llu", first);
		return text;
	}
	for (size_t i = 0; i < effective->count; i++)
		used += (size_t)snprintf(text + used, size - used, "%s%u=%llu",
			i != 0 ? "," : "", effective->cpus[i],
			runtime_for_cpu(runtime, effective->cpus[i]));
	return text;
}

int cmd_list(void)
{
	DIR *dir = opendir(RUN_ROOT);
	if (dir == NULL)
		return 0;

	printf("%-24s %-12s %-8s %-12s %-24s %-12s %-13s %-12s %-8s\n",
		"NAME", "STATE", "PID", "DEFAULT_US", "RUNTIME_US", "PERIOD_US", "TOTAL_BUDGET",
		"CPUS", "RECLAIM");

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		if (strcmp(ent->d_name, "channels") == 0 ||
		    strcmp(ent->d_name, "rootfs") == 0)
			continue;

		pid_t pid = 0;
		unsigned long long starttime = 0;
		if (read_main_meta(ent->d_name, &pid, &starttime) != 0) {
			printf("%-24s %-12s %-8s %-12s %-24s %-12s %-13s %-12s %-8s\n",
				ent->d_name, "initializing", "-", "-", "-", "-", "-", "-", "-");
			continue;
		}

		enum domain_state st = domain_state(ent->d_name, pid, starttime);
		struct runtime_config runtime;
		unsigned long long period = 0;
		char effective[CPU_LIST_SIZE] = "";
		char total_budget[32] = "-";
		char *runtime_text = NULL;
		struct cpu_list effective_cpus = {0};
		int reclaim = 0;
		int cfg = read_config(ent->d_name, &runtime, &period);
		int cpu_cfg = read_cpus(ent->d_name, effective, sizeof(effective));
		int reclaim_cfg = read_reclaim(ent->d_name, &reclaim);
		if (cpu_cfg == 0 && parse_cpu_list(effective, &effective_cpus) != 0)
			cpu_cfg = -1;
		if (cfg == 0 && cpu_cfg == 0) {
			long double total = total_runtime(&runtime, &effective_cpus) * 100.0L /
				(long double)period;

			snprintf(total_budget, sizeof(total_budget), "%.2Lf%%", total);
			runtime_text = format_effective_runtime(&runtime, &effective_cpus);
		}

		printf("%-24s %-12s %-8lld %-12lld %-24s %-12lld %-13s %-12s %-8s\n",
			ent->d_name, state_name(st),
			(long long) pid,
			cfg == 0 ? (long long)runtime.default_runtime : -1,
			runtime_text != NULL ? runtime_text : "-",
			cfg == 0 ? (long long) period : -1,
			total_budget,
			cpu_cfg == 0 ? effective : "-",
			reclaim_cfg == 0 ? (reclaim ? "true" : "false") : "-");
		free(runtime_text);
		free_cpu_list(&effective_cpus);
		free_runtime(&runtime);
	}
	closedir(dir);
	return 0;
}

int cmd_inspect(const char *name)
{
	if (!is_valid_name(name)) {
		fprintf(stderr, "tgbsctl: ERROR - --name must match [a-zA-Z0-9_.-]+ (max 255, no '/'); got '%s'\n",
			name);
		return 2;
	}

	pid_t pid = 0;
	unsigned long long starttime = 0;
	if (read_main_meta(name, &pid, &starttime) != 0) {
		printf("Name:        %s\n", name);
		printf("State:       unknown\n");
		fprintf(stderr, "tgbsctl: ERROR - no runtime metadata for %s\n", name);
		return 1;
	}

	enum domain_state st = domain_state(name, pid, starttime);

	struct runtime_config runtime;
	unsigned long long period = 0;
	int cfg = read_config(name, &runtime, &period);
	char effective[CPU_LIST_SIZE] = "";
	struct cpu_list effective_cpus = {0};
	int cpu_cfg = read_cpus(name, effective, sizeof(effective));
	if (cpu_cfg == 0 && parse_cpu_list(effective, &effective_cpus) != 0)
		cpu_cfg = -1;
	int reclaim = 0;
	int reclaim_cfg = read_reclaim(name, &reclaim);
	char memory_current[32] = "";
	char memory_peak[32] = "";
	char memory_max[32] = "";
	char memory_current_human[32] = "";
	char memory_peak_human[32] = "";
	char memory_max_human[32] = "";
	char pids_current[32] = "";
	char pids_max[32] = "";
	int memory_current_cfg = read_resource_file(name, "memory.current",
		memory_current, sizeof(memory_current));
	int memory_peak_cfg = read_resource_file(name, "memory.peak",
		memory_peak, sizeof(memory_peak));
	int memory_max_cfg = read_resource_file(name, "memory.max",
		memory_max, sizeof(memory_max));
	int pids_current_cfg = read_resource_file(name, "pids.current",
		pids_current, sizeof(pids_current));
	int pids_max_cfg = read_resource_file(name, "pids.max",
		pids_max, sizeof(pids_max));
	if (memory_current_cfg == 0 && format_bytes(memory_current,
			memory_current_human, sizeof(memory_current_human)) != 0)
		memory_current_cfg = -1;
	if (memory_peak_cfg == 0 && format_bytes(memory_peak,
			memory_peak_human, sizeof(memory_peak_human)) != 0)
		memory_peak_cfg = -1;
	if (memory_max_cfg == 0 && format_bytes(memory_max,
			memory_max_human, sizeof(memory_max_human)) != 0)
		memory_max_cfg = -1;
	long double total_budget = 0.0L;
	if (cfg == 0 && cpu_cfg == 0) {
		total_budget = total_runtime(&runtime, &effective_cpus) * 100.0L / period;
	}

	char procs[2048] = "";
	read_procs(name, procs, sizeof(procs));

	char *command = NULL;
	if (main_is_alive(name, pid, starttime))
		command = read_command(pid);

	printf("Name:        %s\n", name);
	printf("State:       %s\n", state_name(st));
	printf("Main PID:    %d\n", (int) pid);
	printf("Default runtime: %lld us\n", cfg == 0 ? (long long)runtime.default_runtime : -1);
	printf("Period:      %lld us\n", cfg == 0 ? (long long) period : -1);
	if (cfg == 0)
		printf("Default budget: %.2Lf %%\n",
			(long double)runtime.default_runtime * 100.0L / period);
	else
		printf("Default budget: (unavailable)\n");
	if (cfg == 0) {
		struct cpu_list possible;

		printf("Runtime/CPU:\n");
		if (read_possible_cpus(&possible) == 0) {
			for (size_t i = 0; i < possible.count; i++) {
				unsigned int cpu = possible.cpus[i];
				unsigned long long value = runtime_for_cpu(&runtime, cpu);
				int enabled = 0;

				for (size_t j = 0; j < effective_cpus.count; j++)
					if (effective_cpus.cpus[j] == cpu) {
						enabled = 1;
						break;
					}
				printf("  CPU %-5u %llu us (%.2Lf %%)%s\n", cpu, value,
					(long double)value * 100.0L / period,
					cpu_cfg == 0 && !enabled ? " [disabled]" : "");
			}
			free_cpu_list(&possible);
		} else {
			printf("  (unavailable)\n");
		}
	}
	printf("CPUs:        %s\n", cpu_cfg == 0 ? effective : "(unavailable)");
	if (cpu_cfg == 0)
		printf("CPU count:   %zu\n", effective_cpus.count);
	else
		printf("CPU count:   (unavailable)\n");
	if (cfg == 0 && cpu_cfg == 0)
		printf("Total budget: %.2Lf %% (%.2Lf CPU, effective CPUs only)\n",
			total_budget, total_budget / 100.0L);
	else
		printf("Total budget: (unavailable)\n");
	printf("Reclaim:     %s\n", reclaim_cfg == 0 ?
		(reclaim ? "true" : "false") : "(unavailable)");
	printf("Memory:      %s\n", memory_current_cfg == 0 ?
		memory_current_human : "(unavailable)");
	printf("Memory peak: %s\n", memory_peak_cfg == 0 ?
		memory_peak_human : "(unavailable)");
	printf("Memory max:  %s\n", memory_max_cfg == 0 ?
		memory_max_human : "(unavailable)");
	printf("PIDs:        %s\n", pids_current_cfg == 0 ?
		pids_current : "(unavailable)");
	printf("PIDs max:    %s\n", pids_max_cfg == 0 ?
		pids_max : "(unavailable)");
	printf("Processes:   %s\n", procs[0] ? procs : "(none)");
	printf("Command:     %s\n", command != NULL ? command : "(not alive)");
	free(command);
	free_cpu_list(&effective_cpus);
	free_runtime(&runtime);
	return 0;
}
