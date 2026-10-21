/* SPDX-License-Identifier: MIT
 * Host file and directory binds retained across rootfs preparation.
 */
#define _GNU_SOURCE
#include "tgbsctl.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int path_below(const char *path, const char *parent)
{
	size_t len = strlen(parent);

	return strncmp(path, parent, len) == 0 &&
		(path[len] == '\0' || path[len] == '/');
}

/* Canonicalize separators and reject traversal before checking private runtime
 * paths. Destination components are opened without following symlinks later. */
static char *parse_destination(char *path)
{
	char normalized[PATH_MAX];
	char *part, *save;
	size_t used = 0;

	if (path[0] != '/' || strlen(path) >= sizeof(normalized))
		goto invalid;
	for (part = strtok_r(path, "/", &save); part != NULL;
	     part = strtok_r(NULL, "/", &save)) {
		size_t len = strlen(part);

		if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0)
			goto invalid;
		normalized[used++] = '/';
		memcpy(normalized + used, part, len);
		used += len;
	}
	normalized[used] = '\0';
	if (used == 0 || path_below(normalized, "/dev") ||
	    path_below(normalized, "/proc") || path_below(normalized, "/sys") ||
	    strcmp(normalized, "/run") == 0 ||
	    path_below(normalized, "/run/lock") ||
	    path_below(normalized, RUN_ROOT) || path_below(RUN_ROOT, normalized) ||
	    path_below(normalized, CHANNEL_ROOT) || path_below(CHANNEL_ROOT, normalized))
		goto invalid;
	return strdup(normalized);
invalid:
	errno = EINVAL;
	return NULL;
}

int bind_mount_plan_add(struct bind_mount_plan *plan, const char *spec)
{
	struct bind_mount_entry entry = {.mount_fd = -1};
	struct bind_mount_entry *entries;
	char *text = strdup(spec);
	char *destination, *mode;
	int saved_errno;

	if (text == NULL)
		return -1;
	destination = strchr(text, ':');
	if (destination == NULL)
		goto invalid;
	*destination++ = '\0';
	mode = strchr(destination, ':');
	if (mode != NULL) {
		*mode++ = '\0';
		if (strcmp(mode, "ro") == 0)
			entry.read_only = 1;
		else if (strcmp(mode, "rw") != 0)
			goto invalid;
	}
	if (text[0] != '/' || strlen(text) >= PATH_MAX)
		goto invalid;
	entry.destination = parse_destination(destination);
	if (entry.destination == NULL)
		goto error;
	for (size_t i = 0; i < plan->count; i++) {
		if (strcmp(entry.destination, plan->entries[i].destination) == 0)
			goto invalid;
	}
	entry.source = strdup(text);
	if (entry.source == NULL)
		goto error;
	entries = reallocarray(plan->entries, plan->count + 1, sizeof(*entries));
	if (entries == NULL)
		goto error;
	plan->entries = entries;
	plan->entries[plan->count++] = entry;
	free(text);
	return 0;
invalid:
	errno = EINVAL;
error:
	saved_errno = errno;
	free(entry.source);
	free(entry.destination);
	free(text);
	errno = saved_errno;
	return -1;
}

void bind_mount_plan_close(struct bind_mount_plan *plan)
{
	for (size_t i = 0; i < plan->count; i++) {
		if (plan->entries[i].mount_fd >= 0)
			close(plan->entries[i].mount_fd);
		free(plan->entries[i].source);
		free(plan->entries[i].destination);
	}
	free(plan->entries);
	memset(plan, 0, sizeof(*plan));
}

int bind_mount_plan_prepare(struct bind_mount_plan *plan)
{
	for (size_t i = 0; i < plan->count; i++) {
		struct bind_mount_entry *entry = &plan->entries[i];
		struct mount_attr attributes = {
			.attr_set = MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV |
				(entry->read_only ? MOUNT_ATTR_RDONLY : 0),
			.propagation = MS_PRIVATE,
		};
		struct stat st;
		int source_fd = open(entry->source, O_PATH | O_CLOEXEC);
		int saved_errno;

		if (source_fd < 0 || fstat(source_fd, &st) != 0)
			goto error;
		if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode) && !S_ISSOCK(st.st_mode)) {
			errno = EINVAL;
			goto error;
		}
		entry->is_directory = S_ISDIR(st.st_mode);
		/* Clone only this path's mount, like mount --bind (not --rbind).
		 * The detached mount remains usable after pivot_root hides the host. */
		entry->mount_fd = (int)syscall(SYS_open_tree, source_fd, "",
			OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
		saved_errno = errno;
		close(source_fd);
		source_fd = -1;
		errno = saved_errno;
		if (entry->mount_fd < 0 ||
		    syscall(SYS_mount_setattr, entry->mount_fd, "", AT_EMPTY_PATH,
			&attributes, sizeof(attributes)) != 0)
			goto error;
		continue;
error:
		saved_errno = errno;
		if (source_fd >= 0)
			close(source_fd);
		fprintf(stderr, "tgbsctl: ERROR - unable to prepare bind source %s: %s\n",
			entry->source, strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static int prepare_file_destination(int parent_fd, const char *name)
{
	struct stat st;
	int saved_errno;
	int fd;

	/* Never truncate existing files or open existing targets for I/O. */
	fd = openat(parent_fd, name,
		O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (fd >= 0)
		close(fd);
	else if (errno != EEXIST)
		return -1;
	fd = openat(parent_fd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0)
		goto error;
	if (!S_ISREG(st.st_mode) && !S_ISSOCK(st.st_mode)) {
		errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
		goto error;
	}
	return fd;
error:
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return -1;
}

/* Resolve every component inside the container after pivot_root. Refusing
 * symlinks also prevents a persistent upper from redirecting a mount target. */
static int prepare_destination(const char *destination, int is_directory)
{
	char path[PATH_MAX];
	char *part, *next_part, *save;
	int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	strcpy(path, destination);
	for (part = strtok_r(path, "/", &save); part != NULL;
	     part = next_part) {
		int next;
		int saved_errno;

		next_part = strtok_r(NULL, "/", &save);
		if (next_part == NULL && !is_directory) {
			next = prepare_file_destination(fd, part);
		} else {
			if (mkdirat(fd, part, 0755) != 0 && errno != EEXIST) {
				saved_errno = errno;
				close(fd);
				errno = saved_errno;
				return -1;
			}
			next = openat(fd, part, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		}
		saved_errno = errno;
		close(fd);
		if (next < 0) {
			errno = saved_errno;
			return -1;
		}
		fd = next;
	}
	return fd;
}

int bind_mount_plan_mount(const struct bind_mount_plan *plan)
{
	for (size_t i = 0; i < plan->count; i++) {
		const struct bind_mount_entry *entry = &plan->entries[i];
		int target_fd = prepare_destination(entry->destination, entry->is_directory);
		int result = -1;
		int saved_errno;

		if (target_fd >= 0) {
			result = (int)syscall(SYS_move_mount, entry->mount_fd, "",
				target_fd, "", MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH);
			saved_errno = errno;
			close(target_fd);
			errno = saved_errno;
		}
		if (result != 0) {
			saved_errno = errno;
			fprintf(stderr, "tgbsctl: ERROR - unable to bind %s at %s: %s\n",
				entry->source, entry->destination, strerror(saved_errno));
			errno = saved_errno;
			return -1;
		}
	}
	return 0;
}
