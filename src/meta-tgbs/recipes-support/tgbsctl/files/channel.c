/*
 * SPDX-License-Identifier: MIT
 *
 * CLI commands for immutable communication-channel contracts.
 * Parse channel options and display results; libtgbs validates contracts
 * and performs channel creation, inspection and deletion.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void print_destinations(const tgbs_channel_info_t *info)
{
    for (size_t i = 0; i < info->destination_count; i++)
        printf("%s%s", i ? "," : "", info->destinations[i]);
}

static int create_channel(int argc, char **argv)
{
    static const struct option options[] = {
        {"name", required_argument, NULL, 'n'},
        {"source", required_argument, NULL, 's'},
        {"destination", required_argument, NULL, 'd'},
        {"max-message-size", required_argument, NULL, 'm'},
        {"type", required_argument, NULL, 't'},
        {"refresh-period-us", required_argument, NULL, 'r'},
        {NULL, 0, NULL, 0}
    };
    const char *destinations[TGBS_CHANNEL_MAX_DESTINATIONS];
    tgbs_channel_spec_t spec = {.destinations = destinations};
    int option;
    optind = 3;
    while ((option = getopt_long(argc, argv, "+n:s:d:m:t:r:", options, NULL)) != -1) {
        switch (option) {
        case 'n': spec.name = optarg; break;
        case 's': spec.source = optarg; break;
        case 'd':
            if (spec.destination_count == TGBS_CHANNEL_MAX_DESTINATIONS) goto invalid;
            destinations[spec.destination_count++] = optarg;
            break;
        case 't':
            if (!strcmp(optarg, "queuing")) spec.type = TGBS_CHANNEL_QUEUING;
            else if (!strcmp(optarg, "sampling")) spec.type = TGBS_CHANNEL_SAMPLING;
            else goto invalid;
            break;
        case 'm': if (cli_u64(optarg, &spec.max_message_size, 1)) goto invalid; break;
        case 'r': if (cli_u64(optarg, &spec.refresh_period_us, 1)) goto invalid; break;
        default: goto invalid;
        }
    }
    if (optind != argc || !spec.name || !spec.source || !spec.type ||
        !spec.destination_count || !spec.max_message_size) goto invalid;
    if (tgbs_channel_create(&spec)) return cli_error("create channel");
    printf("tgbsctl: created %s channel %s\n", tgbs_channel_type_name(spec.type), spec.name);
    return 0;
invalid:
    fprintf(stderr, "tgbsctl: ERROR - invalid channel create options\n");
    return 2;
}

static int list_channels(void)
{
    tgbs_channel_info_t *channels;
    size_t count;
    if (tgbs_channel_list(&channels, &count)) return cli_error("list channels");
    printf("%-20s %-8s %-20s %-12s %-12s %-8s %-8s %s\n", "NAME", "TYPE", "SOURCE",
        "MAX_BYTES", "REFRESH_US", "OPEN", "ENDPOINT", "DESTINATIONS");
    for (size_t i = 0; i < count; i++) {
        tgbs_channel_info_t *info = &channels[i];
        char refresh[32] = "-";
        if (info->type == TGBS_CHANNEL_SAMPLING)
            snprintf(refresh, sizeof(refresh), "%" PRIu64, info->refresh_period_us);
        printf("%-20s %-8s %-20s %-12" PRIu64 " %-12s %-8s %-8s ", info->name,
            tgbs_channel_type_name(info->type), info->source, info->max_message_size,
            refresh, info->open_handles < 0 ? "?" : info->open_handles ? "yes" : "no",
            info->endpoint_present ? "present" : "absent");
        print_destinations(info);
        putchar('\n');
    }
    tgbs_channel_list_free(channels);
    return 0;
}

static int inspect_channel(const char *name)
{
    tgbs_channel_info_t info;
    if (tgbs_channel_inspect(name, &info)) return cli_error("inspect channel");
    printf("Name:             %s\nType:             %s\nGeneration:       %s\n"
           "Source:           %s\nDestination(s):   ", info.name,
           tgbs_channel_type_name(info.type), info.generation, info.source);
    print_destinations(&info);
    putchar('\n');
    if (info.type == TGBS_CHANNEL_SAMPLING)
        printf("Refresh period:   %" PRIu64 " us\n", info.refresh_period_us);
    printf("Max message size: %" PRIu64 " bytes\nOpen handles:     %s\n",
        info.max_message_size, info.open_handles < 0 ? "unknown" : info.open_handles ? "yes" : "no");
    printf("%s   %s\n", info.type == TGBS_CHANNEL_QUEUING ? "Channel socket:" : "Sample file:   ",
        info.endpoint_present ? "present" : "absent");
    printf("Active domain:    %s\n", info.active < 0 ? "(unknown)" :
        info.active ? info.active_domain : "(none)");
    return 0;
}

int cmd_channel(int argc, char **argv)
{
    if (argc >= 3) {
        if (!strcmp(argv[2], "create")) return create_channel(argc, argv);
        if (!strcmp(argv[2], "list") && argc == 3) return list_channels();
        if (!strcmp(argv[2], "inspect") && argc == 4) return inspect_channel(argv[3]);
        if (!strcmp(argv[2], "delete") && argc == 4) {
            if (tgbs_channel_delete(argv[3])) return cli_error("delete channel");
            printf("tgbsctl: deleted channel %s\n", argv[3]);
            return 0;
        }
    }
    fprintf(stderr, "tgbsctl: ERROR - channel requires create, list, inspect, or delete\n");
    return 2;
}
