/* SPDX-License-Identifier: MIT */
#ifndef TGBS_CPU_RUNTIME_H
#define TGBS_CPU_RUNTIME_H

#include <stddef.h>

#ifndef CPU_POSSIBLE_PATH
#define CPU_POSSIBLE_PATH "/sys/devices/system/cpu/possible"
#endif

struct cpu_runtime {
	unsigned int cpu;
	unsigned long long runtime;
};

struct runtime_config {
	unsigned long long default_runtime;
	struct cpu_runtime *overrides;
	size_t count;
};

struct cpu_list {
	unsigned int *cpus;
	size_t count;
};

/* cpu.runtime_us: default plus CPU overrides, or the legacy scalar. */
int read_runtime(const char *path, struct runtime_config *config);
void free_runtime(struct runtime_config *config);
unsigned long long max_runtime(const struct runtime_config *config);
unsigned long long runtime_for_cpu(const struct runtime_config *config,
	unsigned int cpu);

/* Canonical cpulists, including sparse IDs and CPUs which are offline. */
int parse_cpu_list(const char *text, struct cpu_list *list);
int read_possible_cpus(struct cpu_list *list);
void free_cpu_list(struct cpu_list *list);
long double total_runtime(const struct runtime_config *config,
	const struct cpu_list *list);

/* Strict decimal, including zero; rejects signs and trailing characters. */
int parse_runtime_value(const char *text, unsigned long long *out);

#endif
