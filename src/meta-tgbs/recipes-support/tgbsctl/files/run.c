/*
 * SPDX-License-Identifier: MIT
 *
 * Parse container launch options and call libtgbs's blocking run API.
 * The library supervises the domain through cleanup; the CLI handles signals
 * and translates the result into a command-line exit status.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_run(int argc, char **argv)
{
    enum { MEMORY = 256, PIDS, EPHEMERAL, BIND };
    static const struct option options[] = {
        {"name", required_argument, NULL, 'n'},
        {"hostname", required_argument, NULL, 'H'},
        {"runtime-us", required_argument, NULL, 'r'},
        {"period-us", required_argument, NULL, 'p'},
        {"cpus", required_argument, NULL, 'c'},
        {"reclaim", required_argument, NULL, 'R'},
        {"memory-max", required_argument, NULL, MEMORY},
        {"pids-max", required_argument, NULL, PIDS},
        {"ephemeral", no_argument, NULL, EPHEMERAL},
        {"bind", required_argument, NULL, BIND},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    tgbs_domain_spec_t spec = TGBS_DOMAIN_SPEC_INIT;
    tgbs_bind_t *binds = NULL;
    char **bind_texts = NULL;
    size_t count = 0;
    int option, result = 2;
    optind = 2;
    while ((option = getopt_long(argc, argv, "+hH:c:n:r:p:R:", options, NULL)) != -1) {
        switch (option) {
        case 'h': usage(argv[0]); result = 0; goto out;
        case 'n': spec.name = optarg; break;
        case 'H': spec.hostname = optarg; break;
        case 'c': spec.cpus = optarg; break;
        case 'r': if (cli_u64(optarg, &spec.runtime_us, 1)) goto invalid; break;
        case 'p': if (cli_u64(optarg, &spec.period_us, 1)) goto invalid; break;
        case 'R': if (cli_bool(optarg, &spec.reclaim)) goto invalid; break;
        case MEMORY: if (cli_limit(optarg, &spec.memory_max)) goto invalid; break;
        case PIDS: if (cli_limit(optarg, &spec.pids_max)) goto invalid; break;
        case EPHEMERAL: spec.ephemeral = 1; break;
        case BIND: {
            tgbs_bind_t *grown = reallocarray(binds, count + 1, sizeof(*binds));
            if (!grown) { result = cli_error("allocate binds"); goto out; }
            binds = grown;
            char **texts = reallocarray(bind_texts, count + 1, sizeof(*texts));
            if (!texts) { result = cli_error("allocate binds"); goto out; }
            bind_texts = texts;
            char *text = strdup(optarg);
            if (!text) { result = cli_error("allocate bind"); goto out; }
            bind_texts[count] = text;
            count++;
            char *destination = strchr(text, ':');
            if (!destination) goto invalid;
            *destination++ = '\0';
            char *mode = strchr(destination, ':');
            if (mode) *mode++ = '\0';
            if (mode && strcmp(mode, "ro") && strcmp(mode, "rw")) goto invalid;
            binds[count - 1] = (tgbs_bind_t){text, destination,
                mode && strcmp(mode, "ro") == 0};
            break;
        }
        default: goto invalid;
        }
    }
    if (optind >= argc || !spec.name || !spec.runtime_us || !spec.period_us ||
        spec.runtime_us > spec.period_us) goto invalid;
    spec.argv = &argv[optind];
    spec.binds = binds;
    spec.bind_count = count;
    spec.cancel_fd = cli_signals_begin(0);
    if (spec.cancel_fd < 0) { result = cli_error("install signal handling"); goto out; }
    tgbs_exit_status_t status;
    int rc = tgbs_domain_run(&spec, &status);
    int saved = errno;
    result = cli_status(&status);
    cli_signals_end();
    if (rc != 0) {
        errno = saved;
        result = cli_error("run domain");
        if (status.cleanup_errno)
            fprintf(stderr, "tgbsctl: ERROR - cleanup: %s\n", strerror(status.cleanup_errno));
    } else if (status.exited) {
        printf("tgbsctl: command exited with code %d\n", status.exit_code);
    } else if (status.term_signal) {
        printf("tgbsctl: command killed by signal %d\n", status.term_signal);
    }
    goto out;
invalid:
    fprintf(stderr, "tgbsctl: ERROR - invalid run options\n");
    usage(argv[0]);
out:
    for (size_t i = 0; i < count; i++) free(bind_texts[i]);
    free(bind_texts);
    free(binds);
    return result;
}
