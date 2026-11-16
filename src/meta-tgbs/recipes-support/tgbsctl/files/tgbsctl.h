/*
 * SPDX-License-Identifier: MIT
 *
 * Shared declarations for tgbsctl command handlers and CLI helpers.
 * Container and channel APIs are provided by libtgbs.
 */

#ifndef TGBS_CLI_H
#define TGBS_CLI_H
#include <tgbs/domain.h>
#include <tgbs/channel.h>
#include <signal.h>

void usage(const char *program);
int cli_error(const char *operation);
int cli_u64(const char *text, uint64_t *value, int positive);
int cli_bool(const char *text, int *value);
int cli_limit(const char *text, tgbs_limit_t *limit);
int cli_status(const tgbs_exit_status_t *status);
int cli_signals_begin(int attach);
void cli_signals_end(void);
int cmd_run(int argc, char **argv);
int cmd_attach(const char *name);
int cmd_list(void);
int cmd_inspect(const char *name);
int cmd_kill(const char *name);
int cmd_freeze(const char *name, int freeze);
int cmd_set(const char *name, const char *field, const char *text);
int cmd_channel(int argc, char **argv);
#endif
