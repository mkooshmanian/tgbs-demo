/* SPDX-License-Identifier: MIT */
#ifndef TGBS_CHANNEL_H
#define TGBS_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TGBS_CHANNEL_NAME_SIZE 64
#define TGBS_CHANNEL_MAX_DESTINATIONS 64

typedef enum {
    TGBS_CHANNEL_QUEUING = 1,
    TGBS_CHANNEL_SAMPLING = 2
} tgbs_channel_kind_t;

typedef struct {
    const char *name;
    tgbs_channel_kind_t type;
    const char *source;
    const char *const *destinations;
    size_t destination_count;
    uint64_t max_message_size;
    uint64_t refresh_period_us;
} tgbs_channel_spec_t;

typedef struct {
    char name[TGBS_CHANNEL_NAME_SIZE];
    tgbs_channel_kind_t type;
    char generation[64];
    char source[256];
    char destinations[TGBS_CHANNEL_MAX_DESTINATIONS][256];
    size_t destination_count;
    uint64_t max_message_size;
    uint64_t refresh_period_us;
    int open_handles;           /* -1 means unavailable. */
    int endpoint_present;
    int active;                 /* -1 means unavailable. */
    char active_domain[256];
} tgbs_channel_info_t;

/* Contracts remain immutable. Create/delete serialize with domain startup;
 * active participants or open communication handles prevent deletion. */
int tgbs_channel_create(const tgbs_channel_spec_t *spec);
int tgbs_channel_delete(const char *name);
int tgbs_channel_inspect(const char *name, tgbs_channel_info_t *info);
/* The returned array is owned by the caller; release with list_free. */
int tgbs_channel_list(tgbs_channel_info_t **channels, size_t *count);
void tgbs_channel_list_free(tgbs_channel_info_t *channels);
const char *tgbs_channel_type_name(tgbs_channel_kind_t type);

#ifdef __cplusplus
}
#endif
#endif
