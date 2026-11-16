/*
 * SPDX-License-Identifier: MIT
 *
 * tgbsctl mutating commands for active TGBS domains.
 * Parse control values, call libtgbs and report the result. Domain lifecycle
 * supervision and cleanup remain with the blocking run call.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_kill(const char *name)
{
    if (tgbs_domain_kill(name)) return cli_error("kill domain");
    printf("tgbsctl: killed all processes in %s\n", name);
    return 0;
}

int cmd_freeze(const char *name, int freeze)
{
    int rc = freeze ? tgbs_domain_pause(name) : tgbs_domain_resume(name);
    if (rc) return cli_error(freeze ? "pause domain" : "resume domain");
    printf("tgbsctl: %s %s\n", freeze ? "paused" : "resumed", name);
    return 0;
}

static int set_runtimes(const char *name, const char *text)
{
    uint64_t value;
    if (!strchr(text, ',')) {
        if (cli_u64(text, &value, 0)) return 2;
        return tgbs_domain_set_runtime(name, value) ? cli_error("set runtime") : 0;
    }
    char *copy = strdup(text), *cursor = copy, *word;
    uint64_t *values = NULL;
    size_t count = 0;
    int result = 2;
    if (!copy) return cli_error("allocate runtime list");
    while ((word = strsep(&cursor, ","))) {
        if (cli_u64(word, &value, 0)) goto out;
        uint64_t *grown = reallocarray(values, count + 1, sizeof(*values));
        if (!grown) { result = cli_error("allocate runtime list"); goto out; }
        values = grown;
        values[count++] = value;
    }
    result = tgbs_domain_set_cpu_runtimes(name, values, count) ?
        cli_error("set per-CPU runtimes") : 0;
out:
    free(copy);
    free(values);
    return result;
}

int cmd_set(const char *name, const char *field, const char *text)
{
    uint64_t value;
    tgbs_limit_t limit;
    int boolean, rc;
    if (!strcmp(field, "runtime") || !strcmp(field, "runtime-us")) {
        rc = set_runtimes(name, text);
        if (rc) return rc;
    } else if (!strcmp(field, "period")) {
        if (cli_u64(text, &value, 1)) goto invalid;
        if (tgbs_domain_set_period(name, value)) return cli_error("set period");
    } else if (!strcmp(field, "cpus")) {
        if (tgbs_domain_set_cpus(name, text)) return cli_error("set CPUs");
    } else if (!strcmp(field, "reclaim")) {
        if (cli_bool(text, &boolean)) goto invalid;
        if (tgbs_domain_set_reclaim(name, boolean)) return cli_error("set reclaim");
    } else if (!strcmp(field, "memory-max") || !strcmp(field, "pids-max")) {
        if (cli_limit(text, &limit)) goto invalid;
        rc = !strcmp(field, "memory-max") ? tgbs_domain_set_memory_max(name, limit) :
            tgbs_domain_set_pids_max(name, limit);
        if (rc) return cli_error("set resource limit");
    } else goto invalid;
    printf("tgbsctl: set %s for %s to %s\n", field, name, text);
    return 0;
invalid:
    fprintf(stderr, "tgbsctl: ERROR - invalid set field or value\n");
    return 2;
}
