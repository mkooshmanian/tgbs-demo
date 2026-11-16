/*
 * SPDX-License-Identifier: MIT
 *
 * Shared tgbsctl argument parsing, diagnostics and exit-status helpers.
 * CLI signal handlers translate interruptions into cancellation requests
 * for libtgbs through a pipe.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int signal_pipe[2] = {-1, -1};
static volatile sig_atomic_t received_signal;
static struct sigaction previous[4];
static const int handled[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};

int cli_error(const char *operation)
{
    fprintf(stderr, "tgbsctl: ERROR - %s: %s\n", operation, strerror(errno));
    return 1;
}

int cli_u64(const char *text, uint64_t *value, int positive)
{
    char *end;
    unsigned long long raw;
    if (!text || text[0] < '0' || text[0] > '9') goto invalid;
    errno = 0;
    raw = strtoull(text, &end, 10);
    if (errno || *end || (positive && !raw)) goto invalid;
    *value = raw;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

int cli_bool(const char *text, int *value)
{
    if (strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0) *value = 1;
    else if (strcmp(text, "0") == 0 || strcasecmp(text, "false") == 0) *value = 0;
    else { errno = EINVAL; return -1; }
    return 0;
}

int cli_limit(const char *text, tgbs_limit_t *limit)
{
    memset(limit, 0, sizeof(*limit));
    if (strcmp(text, "max") == 0) { limit->unlimited = 1; return 0; }
    return cli_u64(text, &limit->value, 1);
}

static void cancel_handler(int sig)
{
    int saved = errno;
    char byte = 1;
    if (!received_signal) received_signal = sig;
    ssize_t n = write(signal_pipe[1], &byte, 1);
    (void)n;
    errno = saved;
}

int cli_signals_begin(int attach)
{
    if (pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK) != 0) return -1;
    received_signal = 0;
    for (size_t i = 0; i < 4; i++) {
        struct sigaction action = {0};
        action.sa_handler = attach && (handled[i] == SIGINT || handled[i] == SIGQUIT) ?
            SIG_IGN : cancel_handler;
        sigemptyset(&action.sa_mask);
        if (sigaction(handled[i], &action, &previous[i]) != 0) {
            int saved = errno;
            while (i) { i--; sigaction(handled[i], &previous[i], NULL); }
            close(signal_pipe[0]); close(signal_pipe[1]);
            errno = saved;
            return -1;
        }
    }
    return signal_pipe[0];
}

void cli_signals_end(void)
{
    for (size_t i = 0; i < 4; i++) sigaction(handled[i], &previous[i], NULL);
    close(signal_pipe[0]); close(signal_pipe[1]);
    signal_pipe[0] = signal_pipe[1] = -1;
}

int cli_status(const tgbs_exit_status_t *status)
{
    if (status->cancelled && received_signal) return 128 + received_signal;
    return status->exited ? status->exit_code :
        status->term_signal ? 128 + status->term_signal : 1;
}
