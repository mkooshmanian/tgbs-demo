/*
 * SPDX-License-Identifier: MIT
 *
 * Immutable communication-channel administration and mount preparation.
 */

#define _GNU_SOURCE
#include "internal.h"
#include "queuing-format.h"
#include "sampling-format.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <limits.h>
#include <linux/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CONTRACT_FILE "/contract"
#define ENDPOINT_DIR "/endpoint"
#define SOURCE_LOCK "/endpoint/source.lock"
#define DESTINATION_LOCK "/endpoint/destination.lock"
#define LIFETIME_LOCK "/lifetime.lock"

static int make_channel_path(char *out, size_t outsz, const char *name, const char *suffix)
{
	int n = snprintf(out, outsz, "%s/%s%s", CHANNEL_ROOT, name, suffix);

	if (n < 0 || (size_t)n >= outsz) {
		errno = ENAMETOOLONG;
		return -1;
	}

	return 0;
}

static int ensure_runtime_paths(void)
{
	if (mkdir(RUN_ROOT, 0755) != 0 && errno != EEXIST) {
		return -1;
	}

	if (mkdir(CHANNEL_ROOT, 0755) != 0 && errno != EEXIST) {
		return -1;
	}

	return 0;
}

int channel_topology_lock(int exclusive)
{
	int fd;

	if (exclusive && ensure_runtime_paths() != 0) {
		return -1;
	}

	fd = open(CHANNEL_TOPOLOGY_LOCK,
		(exclusive ? O_RDWR | O_CREAT : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}

	if (flock(fd, exclusive ? LOCK_EX : LOCK_SH) != 0) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}

	return fd;
}

void channel_topology_unlock(int fd)
{
	if (fd >= 0) {
		close(fd);
	}
}

static int is_valid_channel_name(const char *name)
{
	if (name == NULL || !is_valid_name(name)) {
		return 0;
	}

	return strlen(name) < CHANNEL_NAME_SIZE;
}

static int read_contract(const char *name, struct tgbs_channel_contract *contract)
{
	return tgbs_contract_read(CHANNEL_ROOT, name, contract);
}

static int read_generation(char *out, size_t outsz)
{
	if (read_text("/proc/sys/kernel/random/uuid", out, outsz) != 0) {
		return -1;
	}

	if (out[0] == '\0') {
		errno = EIO;
		return -1;
	}

	return 0;
}

static int write_contract(const char *name, const struct tgbs_channel_contract *contract)
{
	char path[PATH_MAX];
	FILE *file;
	int fd;
	int failed = 0;

	if (make_channel_path(path, sizeof(path), name, CONTRACT_FILE) != 0) {
		return -1;
	}

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}

	file = fdopen(fd, "w");
	if (file == NULL) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}

	if (fprintf(file,
		    "format=%d\n"
		    "type=%s\n"
		    "name=%s\n"
		    "generation=%s\n"
		    "source=%s\n"
		    "max_message_size=%llu\n",
		    TGBS_CONTRACT_FORMAT, tgbs_contract_type_name(contract->type), contract->name,
		    contract->generation, contract->source, contract->max_message_size) < 0) {
		failed = 1;
	}

	for (size_t i = 0; !failed && i < contract->destination_count; i++) {
		if (fprintf(file, "destination=%s\n", contract->destinations[i]) < 0) {
			failed = 1;
		}
	}

	if (!failed && contract->type == TGBS_CONTRACT_SAMPLING &&
		fprintf(file, "refresh_period_us=%llu\n", contract->refresh_period_us) < 0) {
		failed = 1;
	}

	if (!failed && fflush(file) != 0) {
		failed = 1;
	}

	if (!failed && fsync(fd) != 0) {
		failed = 1;
	}

	if (!failed && fchmod(fd, 0444) != 0) {
		failed = 1;
	}

	if (fclose(file) != 0) {
		failed = 1;
	}

	if (failed) {
		int saved_errno = errno != 0 ? errno : EIO;

		unlink(path);
		errno = saved_errno;
		return -1;
	}

	return 0;
}

/* Return 1 for a populated domain, 0 for an absent or empty domain, and -1
 * when an existing cgroup cannot be inspected. */
static int domain_populated(const char *name)
{
	char path[PATH_MAX];
	char line[128];
	FILE *file;
	struct stat st;

	/* The host is not a container participant, even if an unrelated cgroup
	 * happens to use its reserved name. Open handles retain lifetime.lock. */
	if (strcmp(tgbs_contract_canonical_domain(name), HOST_PARTICIPANT) == 0) {
		return 0;
	}

	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
	if (stat(path, &st) != 0) {
		if (errno == ENOENT) {
			return 0;
		}

		return -1;
	}

	if (!S_ISDIR(st.st_mode)) {
		errno = EINVAL;
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%s/cgroup.events", CG_ROOT, name);
	file = fopen(path, "r");
	if (file == NULL) {
		return -1;
	}
	while (fgets(line, sizeof(line), file) != NULL) {
		int populated;

		if (sscanf(line, "populated %d", &populated) == 1) {
			fclose(file);
			return populated != 0;
		}
	}

	fclose(file);
	errno = EIO;
	return -1;
}

static int contract_has_active_domain(
	const struct tgbs_channel_contract *contract, const char **active_name)
{
	int active = domain_populated(contract->source);

	if (active < 0) {
		return -1;
	}

	if (active) {
		*active_name = contract->source;
		return 1;
	}

	for (size_t i = 0; i < contract->destination_count; i++) {
		active = domain_populated(contract->destinations[i]);
		if (active < 0) {
			return -1;
		}

		if (active) {
			*active_name = contract->destinations[i];
			return 1;
		}
	}

	return 0;
}

static int lifetime_lock(const char *name, int exclusive, int nonblock)
{
	char path[PATH_MAX];
	int operation = exclusive ? LOCK_EX : LOCK_SH;
	int fd;

	if (nonblock) {
		operation |= LOCK_NB;
	}

	if (make_channel_path(path, sizeof(path), name, LIFETIME_LOCK) != 0) {
		return -1;
	}

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		return -1;
	}

	if (flock(fd, operation) != 0) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}

	return fd;
}

static int channel_has_open_handles(const char *name)
{
	int fd = lifetime_lock(name, 1, 1);

	if (fd >= 0) {
		close(fd);
		return 0;
	}

	if (errno == EWOULDBLOCK) {
		return 1;
	}

	return -1;
}

static int channel_socket_exists(const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	if (make_channel_path(path, sizeof(path), name, TGBS_QUEUING_SOCKET) != 0) {
		return 0;
	}

	return lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int channel_sample_exists(const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	if (make_channel_path(path, sizeof(path), name, TGBS_SAMPLING_FILE) != 0) {
		return 0;
	}

	return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Called only with exclusive topology/lifetime locks and inactive participants.
 * Remove the published sample and exact temporary names left by crashed writers;
 * unrelated files are never removed. */
static int remove_sampling_files(const char *name)
{
	char path[PATH_MAX];

	DIR *dir;
	int saved_errno = 0;
	int deleting = 0;

	if (make_channel_path(path, sizeof(path), name, ENDPOINT_DIR) != 0) {
		return -1;
	}

	dir = opendir(path);
	if (dir == NULL) {
		return -1;
	}

	for (;;) {
		struct dirent *entry;
		struct stat st;

		errno = 0;
		entry = readdir(dir);
		if (entry == NULL) {
			saved_errno = errno;
			if (saved_errno == 0 && !deleting) {

				/* Validate the complete directory before removing any data. */
				deleting = 1;
				rewinddir(dir);
				continue;
			}

			break;
		}

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		if (strcmp(entry->d_name, "source.lock") != 0 &&
			strcmp(entry->d_name, TGBS_SAMPLING_FILE_NAME) != 0 &&
			!tgbs_sampling_is_temporary(entry->d_name)) {
			saved_errno = ENOTEMPTY;
			break;
		}

		if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
			saved_errno = errno;
			break;
		}

		if (!S_ISREG(st.st_mode)) {
			saved_errno = EINVAL;
			break;
		}

		if (deleting && strcmp(entry->d_name, "source.lock") != 0 &&
			unlinkat(dirfd(dir), entry->d_name, 0) != 0) {
			saved_errno = errno;
			break;
		}
	}

	if (closedir(dir) != 0 && saved_errno == 0) {
		saved_errno = errno;
	}

	if (saved_errno != 0) {
		errno = saved_errno;
		return -1;
	}

	return 0;
}

static int remove_if_exists(const char *path)
{
	if (unlink(path) == 0 || errno == ENOENT) {
		return 0;
	}

	return -1;
}

static int create_lock_file(const char *name, const char *suffix)
{
	char path[PATH_MAX];
	int fd;

	if (make_channel_path(path, sizeof(path), name, suffix) != 0) {
		return -1;
	}

	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}

	if (close(fd) != 0) {
		return -1;
	}

	return 0;
}

static void cleanup_partial_channel(const char *name)
{
	char path[PATH_MAX];

	if (make_channel_path(path, sizeof(path), name, CONTRACT_FILE) == 0) {
		unlink(path);
	}

	if (make_channel_path(path, sizeof(path), name, LIFETIME_LOCK) == 0) {
		unlink(path);
	}

	if (make_channel_path(path, sizeof(path), name, SOURCE_LOCK) == 0) {
		unlink(path);
	}

	if (make_channel_path(path, sizeof(path), name, DESTINATION_LOCK) == 0) {
		unlink(path);
	}

	if (make_channel_path(path, sizeof(path), name, ENDPOINT_DIR) == 0) {
		rmdir(path);
	}

	if (make_channel_path(path, sizeof(path), name, "") == 0) {
		rmdir(path);
	}
}

int tgbs_channel_delete(const char *name)
{
	struct tgbs_channel_contract contract;
	const char *active_name = NULL;
	char path[PATH_MAX];
	struct stat st;
	int topology_fd;
	int lifetime_fd = -1;
	int active;
	int rc = -1;
	int saved_errno;

	if (!is_valid_channel_name(name)) {
		errno = EINVAL;
		return -1;
	}

	topology_fd = channel_topology_lock(1);
	if (topology_fd < 0) {
		return -1;
	}

	if (read_contract(name, &contract) != 0) {
		goto out_topology;
	}

	lifetime_fd = lifetime_lock(name, 1, 1);
	if (lifetime_fd < 0) {
		goto out_topology;
	}

	active = contract_has_active_domain(&contract, &active_name);
	if (active < 0) {
		goto out_topology;
	}

	if (active) {
		errno = EBUSY;
		goto out_topology;
	}

	if (make_channel_path(path, sizeof(path), name, "") != 0 || chmod(path, 0755) != 0) {
		goto out_topology;
	}

	if (contract.type == TGBS_CONTRACT_QUEUING) {
		if (make_channel_path(path, sizeof(path), name, TGBS_QUEUING_SOCKET) != 0) {
			goto out_topology;
		}

		if (lstat(path, &st) == 0) {
			if (!S_ISSOCK(st.st_mode)) {
				errno = EINVAL;
				goto out_topology;
			}

			if (unlink(path) != 0) {
				goto out_topology;
			}
		} else if (errno != ENOENT) {
			goto out_topology;
		}
	} else if (remove_sampling_files(name) != 0) {
		goto out_topology;
	}

	if (make_channel_path(path, sizeof(path), name, SOURCE_LOCK) != 0 ||
		remove_if_exists(path) != 0) {
		goto out_topology;
	}

	if (make_channel_path(path, sizeof(path), name, DESTINATION_LOCK) != 0 ||
		remove_if_exists(path) != 0) {
		goto out_topology;
	}

	if (make_channel_path(path, sizeof(path), name, ENDPOINT_DIR) != 0 || rmdir(path) != 0) {
		goto out_topology;
	}

	if (make_channel_path(path, sizeof(path), name, CONTRACT_FILE) != 0 || unlink(path) != 0) {
		goto out_topology;
	}

	if (make_channel_path(path, sizeof(path), name, LIFETIME_LOCK) != 0 || unlink(path) != 0) {
		goto out_topology;
	}

	close(lifetime_fd);
	lifetime_fd = -1;
	if (make_channel_path(path, sizeof(path), name, "") != 0 || rmdir(path) != 0) {
		goto out_topology;
	}

	rc = 0;
	goto out_topology;

out_topology:
	saved_errno = errno;
	if (lifetime_fd >= 0) {
		close(lifetime_fd);
	}

	channel_topology_unlock(topology_fd);
	errno = saved_errno;
	return rc;
}

void channel_mount_plan_close(struct channel_mount_plan *plan)
{
	size_t i;

	if (plan == NULL) {
		return;
	}

	for (i = 0; i < plan->count; i++) {
		if (plan->entries[i].channel_fd >= 0) {
			close(plan->entries[i].channel_fd);
		}

		if (plan->entries[i].endpoint_fd >= 0) {
			close(plan->entries[i].endpoint_fd);
		}

		if (plan->entries[i].source_lock_fd >= 0) {
			close(plan->entries[i].source_lock_fd);
		}
	}

	free(plan->entries);
	plan->entries = NULL;
	plan->count = 0;
}

static int open_channel_mount_path(const char *name, const char *suffix)
{
	char path[PATH_MAX];

	if (make_channel_path(path, sizeof(path), name, suffix) != 0) {
		return -1;
	}

	return (int)syscall(SYS_open_tree, AT_FDCWD, path,
		OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW);
}

int channel_mount_plan_prepare(const char *domain, struct channel_mount_plan *plan)
{
	DIR *dir;
	struct dirent *entry;
	int saved_errno;

	if (domain == NULL || plan == NULL) {
		errno = EINVAL;
		return -1;
	}

	memset(plan, 0, sizeof(*plan));

	dir = opendir(CHANNEL_ROOT);
	if (dir == NULL) {
		if (errno == ENOENT) {
			return 0;
		}

		return -1;
	}
	while ((entry = readdir(dir)) != NULL) {
		struct tgbs_channel_contract contract;
		struct channel_mount_entry mount_entry = {
			.channel_fd = -1,
			.endpoint_fd = -1,
			.source_lock_fd = -1,
		};

		struct channel_mount_entry *new_entries;

		if (entry->d_name[0] == '.') {
			continue;
		}

		if (read_contract(entry->d_name, &contract) != 0) {
			errno = EINVAL;
			goto error;
		}

		if (strcmp(contract.source, domain) == 0) {
			mount_entry.roles |= CHANNEL_MOUNT_SOURCE;
		}

		if (tgbs_contract_has_destination(&contract, domain)) {
			mount_entry.roles |= CHANNEL_MOUNT_DESTINATION;
		}

		if (mount_entry.roles == 0) {
			continue;
		}

		snprintf(mount_entry.name, sizeof(mount_entry.name), "%s", contract.name);
		mount_entry.type = contract.type;
		mount_entry.channel_fd = open_channel_mount_path(contract.name, "");
		if (mount_entry.channel_fd < 0) {
			goto entry_error;
		}

		mount_entry.source_lock_fd = open_channel_mount_path(contract.name, SOURCE_LOCK);
		if (mount_entry.source_lock_fd < 0) {
			goto entry_error;
		}

		if ((contract.type == TGBS_CONTRACT_QUEUING &&
			    (mount_entry.roles & CHANNEL_MOUNT_DESTINATION)) ||
			(contract.type == TGBS_CONTRACT_SAMPLING &&
				(mount_entry.roles & CHANNEL_MOUNT_SOURCE))) {
			mount_entry.endpoint_fd =
				open_channel_mount_path(contract.name, ENDPOINT_DIR);
			if (mount_entry.endpoint_fd < 0) {
				goto entry_error;
			}
		}

		new_entries = realloc(plan->entries, (plan->count + 1) * sizeof(*plan->entries));
		if (new_entries == NULL) {
			goto entry_error;
		}

		plan->entries = new_entries;
		plan->entries[plan->count++] = mount_entry;
		continue;

	entry_error:
		saved_errno = errno;
		if (mount_entry.channel_fd >= 0) {
			close(mount_entry.channel_fd);
		}

		if (mount_entry.endpoint_fd >= 0) {
			close(mount_entry.endpoint_fd);
		}

		if (mount_entry.source_lock_fd >= 0) {
			close(mount_entry.source_lock_fd);
		}

		errno = saved_errno;
		goto error;
	}

	closedir(dir);
	return 0;

error:
	saved_errno = errno;
	closedir(dir);
	channel_mount_plan_close(plan);
	errno = saved_errno;
	return -1;
}

const char *tgbs_channel_type_name(tgbs_channel_kind_t type)
{
	return tgbs_contract_type_name((enum tgbs_contract_type)type);
}

int tgbs_channel_create(const tgbs_channel_spec_t *spec)
{
	struct tgbs_channel_contract contract = { 0 };
	const char *active_name;
	char path[PATH_MAX];
	int lock = -1, owned = 0, result = -1, saved;

	if (spec == NULL || !is_valid_channel_name(spec->name) || !spec->source ||
		!spec->destinations || spec->destination_count == 0 ||
		spec->destination_count > TGBS_CHANNEL_MAX_DESTINATIONS ||
		!tgbs_contract_valid_name(
			tgbs_contract_canonical_domain(spec->source), sizeof(contract.source))) {
		errno = EINVAL;
		return -1;
	}

	/* Build and validate the immutable contract before changing topology. */
	strcpy(contract.name, spec->name);
	strcpy(contract.source, tgbs_contract_canonical_domain(spec->source));
	contract.type = (enum tgbs_contract_type)spec->type;
	contract.max_message_size = spec->max_message_size;
	contract.refresh_period_us = spec->refresh_period_us;
	for (size_t i = 0; i < spec->destination_count; i++) {
		if (tgbs_contract_add_destination(&contract, spec->destinations[i]) != 0) {
			return -1;
		}
	}

	if (read_generation(contract.generation, sizeof(contract.generation)) != 0 ||
		tgbs_contract_validate(&contract) != 0) {
		return -1;
	}

	/* Participants must be inactive while their topology changes. */
	lock = channel_topology_lock(1);
	if (lock < 0) {
		return -1;
	}

	int active = contract_has_active_domain(&contract, &active_name);

	if (active < 0) {
		goto out;
	}

	if (active) {
		errno = EBUSY;
		goto out;
	}

	if (make_channel_path(path, sizeof(path), spec->name, "") != 0 || mkdir(path, 0755) != 0) {
		goto out;
	}

	owned = 1;

	/* Create the endpoint and lifetime/role locks before publishing the
	 * contract. Partial failures remove only the directory created here. */
	if (make_channel_path(path, sizeof(path), spec->name, ENDPOINT_DIR) != 0 ||
		mkdir(path, 0755) != 0 || create_lock_file(spec->name, LIFETIME_LOCK) != 0 ||
		create_lock_file(spec->name, SOURCE_LOCK) != 0) {
		goto out;
	}

	if (contract.type == TGBS_CONTRACT_QUEUING &&
		create_lock_file(spec->name, DESTINATION_LOCK) != 0) {
		goto out;
	}

	if (write_contract(spec->name, &contract) != 0 ||
		make_channel_path(path, sizeof(path), spec->name, "") != 0 ||
		chmod(path, 0555) != 0) {
		goto out;
	}

	result = 0;

out:
	saved = errno;
	if (result && owned) {
		cleanup_partial_channel(spec->name);
	}

	channel_topology_unlock(lock);
	errno = saved;
	return result;
}

static int inspect_locked(const char *name, tgbs_channel_info_t *info)
{
	struct tgbs_channel_contract contract;
	const char *active_name = NULL;

	if (read_contract(name, &contract) != 0) {
		return -1;
	}

	memset(info, 0, sizeof(*info));
	strcpy(info->name, contract.name);
	strcpy(info->generation, contract.generation);
	strcpy(info->source, contract.source);
	info->type = (tgbs_channel_kind_t)contract.type;
	info->destination_count = contract.destination_count;
	memcpy(info->destinations, contract.destinations, sizeof(info->destinations));
	info->max_message_size = contract.max_message_size;
	info->refresh_period_us = contract.refresh_period_us;
	info->open_handles = channel_has_open_handles(name);
	info->active = contract_has_active_domain(&contract, &active_name);
	if (active_name) {
		strcpy(info->active_domain, active_name);
	}

	info->endpoint_present = contract.type == TGBS_CONTRACT_QUEUING
		? channel_socket_exists(name)
		: channel_sample_exists(name);
	return 0;
}

int tgbs_channel_inspect(const char *name, tgbs_channel_info_t *info)
{
	if (info == NULL || !is_valid_channel_name(name)) {
		errno = EINVAL;
		return -1;
	}

	int lock = channel_topology_lock(0);

	if (lock < 0) {
		return -1;
	}

	int result = inspect_locked(name, info), saved = errno;

	channel_topology_unlock(lock);
	errno = saved;
	return result;
}

void tgbs_channel_list_free(tgbs_channel_info_t *channels)
{
	free(channels);
}

int tgbs_channel_list(tgbs_channel_info_t **out, size_t *count)
{
	DIR *dir = NULL;
	struct dirent *entry;
	int saved, lock;

	if (out) {
		*out = NULL;
	}

	if (count) {
		*count = 0;
	}

	if (out == NULL || !count) {
		errno = EINVAL;
		return -1;
	}

	lock = channel_topology_lock(0);
	if (lock < 0) {
		return errno == ENOENT ? 0 : -1;
	}

	dir = opendir(CHANNEL_ROOT);
	if (dir == NULL) {
		saved = errno;
		channel_topology_unlock(lock);
		errno = saved;
		return saved == ENOENT ? 0 : -1;
	}

	for (;;) {
		errno = 0;
		entry = readdir(dir);
		if (!entry) {
			if (errno) {
				goto fail;
			}

			break;
		}

		if (entry->d_name[0] == '.') {
			continue;
		}

		if (!is_valid_channel_name(entry->d_name)) {
			errno = EPROTO;
			goto fail;
		}

		tgbs_channel_info_t info;

		if (inspect_locked(entry->d_name, &info) != 0) {
			goto fail;
		}

		tgbs_channel_info_t *grown = reallocarray(*out, *count + 1, sizeof(**out));

		if (grown == NULL) {
			goto fail;
		}

		*out = grown;
		(*out)[(*count)++] = info;
	}

	closedir(dir);
	channel_topology_unlock(lock);
	return 0;

fail:
	saved = errno;
	closedir(dir);
	channel_topology_unlock(lock);
	free(*out);
	*out = NULL;
	*count = 0;
	errno = saved;
	return -1;
}
