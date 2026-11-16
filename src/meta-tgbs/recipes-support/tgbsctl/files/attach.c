/*
 * SPDX-License-Identifier: MIT
 *
 * Open a shell in an existing domain without changing its lifecycle.
 * libtgbs executes the shell in the domain; the CLI prepares its prompt,
 * handles signals and restores the terminal on return.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

int cmd_attach(const char *name)
{
    char *argv[] = { "/bin/sh", NULL };
    tgbs_exec_spec_t spec = TGBS_EXEC_SPEC_INIT;
    tgbs_exit_status_t status;
    struct termios terminal;
    pid_t foreground = tcgetpgrp(STDIN_FILENO);
    int have_terminal = tcgetattr(STDIN_FILENO, &terminal) == 0;
    char *prompt;

    if (asprintf(&prompt, "[tgbs:%s] # ", name) < 0) {
        return cli_error("allocate prompt");
    }

    int rc = setenv("PS1", prompt, 1);
    free(prompt);

    if (rc) {
        return cli_error("set shell prompt");
    }

    spec.argv = argv;
    spec.cancel_fd = cli_signals_begin(1);

    if (spec.cancel_fd < 0) {
        return cli_error("install signal handling");
    }

    rc = tgbs_domain_exec(name, &spec, &status);
    int saved = errno;
    int result = cli_status(&status);

    cli_signals_end();

    struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;

    sigemptyset(&ignore.sa_mask);
    sigaction(SIGTTOU, &ignore, &previous);

    if (foreground >= 0) {
        tcsetpgrp(STDIN_FILENO, foreground);
    }

    if (have_terminal) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &terminal);
    }

    sigaction(SIGTTOU, &previous, NULL);

    if (rc) {
        errno = saved;
        return cli_error("attach domain");
    }

    return result;
}
