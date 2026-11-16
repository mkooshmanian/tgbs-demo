/*
 * SPDX-License-Identifier: MIT
 *
 * tgbsctl list / inspect -- read-only observation of TGBS domains.
 * Format domain snapshots returned by libtgbs for command-line output.
 */

#include "tgbsctl.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *effective_runtime(const tgbs_domain_info_t *info)
{
    size_t size = 32 + info->cpu_count * 48, used = 0;
    char *text = malloc(size);
    uint64_t first = 0;
    int found = 0, uniform = 1;
    if (!text) return NULL;
    strcpy(text, "-");
    for (size_t i = 0; i < info->cpu_count; i++) {
        if (!info->cpu_budgets[i].enabled) continue;
        uint64_t value = info->cpu_budgets[i].runtime_us;
        if (!found) first = value;
        else if (value != first) uniform = 0;
        found = 1;
    }
    if (!found) return text;
    if (uniform) { snprintf(text, size, "%" PRIu64, first); return text; }
    for (size_t i = 0; i < info->cpu_count; i++) {
        const tgbs_cpu_budget_t *budget = &info->cpu_budgets[i];
        if (!budget->enabled) continue;
        used += snprintf(text + used, size - used, "%s%u=%" PRIu64,
                         used ? "," : "", budget->cpu, budget->runtime_us);
    }
    return text;
}

int cmd_list(void)
{
    tgbs_domain_info_t **domains;
    size_t count;
    if (tgbs_domain_list(&domains, &count)) return cli_error("list domains");
    printf("%-24s %-12s %-8s %-12s %-24s %-12s %-13s %-12s %-8s\n",
        "NAME", "STATE", "PID", "DEFAULT_US", "RUNTIME_US", "PERIOD_US",
        "TOTAL_BUDGET", "CPUS", "RECLAIM");
    for (size_t i = 0; i < count; i++) {
        tgbs_domain_info_t *info = domains[i];
        char total[32] = "-";
        char *budgets = effective_runtime(info);
        unsigned int required = TGBS_INFO_CONTRACT | TGBS_INFO_CPUS;
        if ((info->available & required) == required)
            snprintf(total, sizeof(total), "%.2Lf%%", info->total_budget_percent);
        printf("%-24s %-12s %-8d %-12lld %-24s %-12lld %-13s %-12s %-8s\n",
            info->name, tgbs_domain_state_name(info->state), info->main_pid,
            info->available & TGBS_INFO_CONTRACT ? (long long)info->default_runtime_us : -1,
            budgets ? budgets : "-",
            info->available & TGBS_INFO_CONTRACT ? (long long)info->period_us : -1,
            total, info->available & TGBS_INFO_CPUS ? info->cpus : "-",
            info->available & TGBS_INFO_RECLAIM ? (info->reclaim ? "true" : "false") : "-");
        free(budgets);
    }
    tgbs_domain_list_free(domains, count);
    return 0;
}

static void print_bytes(const char *label, uint64_t bytes, int available, int unlimited)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    if (!available) { printf("%-13s(unavailable)\n", label); return; }
    if (unlimited) { printf("%-13smax\n", label); return; }
    size_t unit = 0;
    long double value = bytes;
    while (value >= 1024 && unit < 6) { value /= 1024; unit++; }
    if (!unit) printf("%-13s%" PRIu64 " B\n", label, bytes);
    else printf("%-13s%.2Lf %s\n", label, value, units[unit]);
}

int cmd_inspect(const char *name)
{
    tgbs_domain_info_t *info;
    if (tgbs_domain_inspect(name, &info)) return cli_error("inspect domain");
    unsigned int flags = info->available;
    printf("Name:        %s\nState:       %s\nMain PID:    %d\n", info->name,
           tgbs_domain_state_name(info->state), info->main_pid);
    printf("Default runtime: %lld us\nPeriod:      %lld us\n",
        flags & TGBS_INFO_CONTRACT ? (long long)info->default_runtime_us : -1,
        flags & TGBS_INFO_CONTRACT ? (long long)info->period_us : -1);
    if (flags & TGBS_INFO_CONTRACT) {
        printf("Default budget: %.2Lf %%\nRuntime/CPU:\n",
            (long double)info->default_runtime_us * 100 / info->period_us);
        for (size_t i = 0; i < info->cpu_count; i++) {
            const tgbs_cpu_budget_t *budget = &info->cpu_budgets[i];
            printf("  CPU %-5u %" PRIu64 " us (%.2Lf %%)%s\n", budget->cpu,
                budget->runtime_us, (long double)budget->runtime_us * 100 / info->period_us,
                (flags & TGBS_INFO_CPUS) && !budget->enabled ? " [disabled]" : "");
        }
    }
    printf("CPUs:        %s\n", flags & TGBS_INFO_CPUS ? info->cpus : "(unavailable)");
    size_t enabled = 0;
    for (size_t i = 0; i < info->cpu_count; i++) enabled += info->cpu_budgets[i].enabled;
    printf("CPU count:   %zu\n", enabled);
    if ((flags & (TGBS_INFO_CONTRACT | TGBS_INFO_CPUS)) ==
        (TGBS_INFO_CONTRACT | TGBS_INFO_CPUS))
        printf("Total budget: %.2Lf %% (%.2Lf CPU, effective CPUs only)\n",
            info->total_budget_percent, info->total_budget_percent / 100);
    else printf("Total budget: (unavailable)\n");
    printf("Reclaim:     %s\n", flags & TGBS_INFO_RECLAIM ?
        (info->reclaim ? "true" : "false") : "(unavailable)");
    print_bytes("Memory:", info->memory_current, flags & TGBS_INFO_MEMORY_CURRENT, 0);
    print_bytes("Memory peak:", info->memory_peak, flags & TGBS_INFO_MEMORY_PEAK, 0);
    print_bytes("Memory max:", info->memory_max.value, flags & TGBS_INFO_MEMORY_MAX,
                info->memory_max.unlimited);
    if (flags & TGBS_INFO_PIDS_CURRENT) printf("PIDs:        %" PRIu64 "\n", info->pids_current);
    else printf("PIDs:        (unavailable)\n");
    if (!(flags & TGBS_INFO_PIDS_MAX)) printf("PIDs max:    (unavailable)\n");
    else if (info->pids_max.unlimited) printf("PIDs max:    max\n");
    else printf("PIDs max:    %" PRIu64 "\n", info->pids_max.value);
    printf("Processes:   ");
    for (size_t i = 0; i < info->process_count; i++)
        printf("%s%d", i ? " " : "", info->processes[i]);
    printf("%s\nCommand:     %s\n", info->process_count ? "" : "(none)",
        info->command ? info->command : "(not alive)");
    tgbs_domain_info_free(info);
    return 0;
}
