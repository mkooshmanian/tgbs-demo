/*
 * SPDX-License-Identifier: MIT
 *
 * Immutable communication-channel contracts managed by tgbsctl.
 */

#define _GNU_SOURCE
#include "tgbsctl.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CONTRACT_FORMAT 1
#define CONTRACT_FILE "/contract"
#define ENDPOINT_DIR "/endpoint"
#define CHANNEL_SOCKET "/endpoint/channel.sock"
#define SOURCE_LOCK "/endpoint/source.lock"
#define RECEIVER_LOCK "/endpoint/receiver.lock"
#define LIFETIME_LOCK "/lifetime.lock"

struct channel_contract {
	char name[CHANNEL_NAME_SIZE];
	char generation[64];
	char source[256];
	char destination[256];
	unsigned long long max_message_size;
};

static int make_channel_path(char *out, size_t outsz, const char *name,
		const char *suffix)
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
	if (mkdir(RUN_ROOT, 0755) != 0 && errno != EEXIST)
		return -1;
	if (mkdir(CHANNEL_ROOT, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

int channel_topology_lock(int exclusive)
{
	int fd;

	if (ensure_runtime_paths() != 0)
		return -1;
	fd = open(CHANNEL_TOPOLOGY_LOCK,
		O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -1;
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
	if (fd >= 0)
		close(fd);
}

static int is_valid_channel_name(const char *name)
{
	if (name == NULL || !is_valid_name(name))
		return 0;
	return strlen(name) < CHANNEL_NAME_SIZE;
}

static int copy_value(char *out, size_t outsz, const char *value)
{
	size_t len = strlen(value);

	if (len == 0 || len >= outsz) {
		errno = EINVAL;
		return -1;
	}
	memcpy(out, value, len + 1);
	return 0;
}

static int read_contract(const char *name, struct channel_contract *contract)
{
	char path[PATH_MAX];
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length;
	FILE *file;
	unsigned int seen = 0;
	unsigned long long format = 0;

	if (!is_valid_channel_name(name)) {
		errno = EINVAL;
		return -1;
	}
	if (make_channel_path(path, sizeof(path), name, CONTRACT_FILE) != 0)
		return -1;
	file = fopen(path, "r");
	if (file == NULL)
		return -1;

	memset(contract, 0, sizeof(*contract));
	while ((length = getline(&line, &capacity, file)) >= 0) {
		char *equals;
		char *key;
		char *value;

		while (length > 0 &&
		       (line[length - 1] == '\n' || line[length - 1] == '\r'))
			line[--length] = '\0';
		equals = strchr(line, '=');
		if (equals == NULL || equals == line || equals[1] == '\0')
			goto invalid;
		*equals = '\0';
		key = line;
		value = equals + 1;

		if (strcmp(key, "format") == 0 && !(seen & 1U)) {
			if (parse_positive(value, &format) != 0)
				goto invalid;
			seen |= 1U;
		} else if (strcmp(key, "name") == 0 && !(seen & 2U)) {
			if (copy_value(contract->name, sizeof(contract->name), value) != 0)
				goto invalid;
			seen |= 2U;
		} else if (strcmp(key, "generation") == 0 && !(seen & 4U)) {
			if (copy_value(contract->generation,
					sizeof(contract->generation), value) != 0)
				goto invalid;
			seen |= 4U;
		} else if (strcmp(key, "source") == 0 && !(seen & 8U)) {
			if (copy_value(contract->source,
					sizeof(contract->source), value) != 0)
				goto invalid;
			seen |= 8U;
		} else if (strcmp(key, "destination") == 0 && !(seen & 16U)) {
			if (copy_value(contract->destination,
					sizeof(contract->destination), value) != 0)
				goto invalid;
			seen |= 16U;
		} else if (strcmp(key, "max_message_size") == 0 && !(seen & 32U)) {
			if (parse_positive(value, &contract->max_message_size) != 0)
				goto invalid;
			seen |= 32U;
		} else {
			goto invalid;
		}
	}
	if (ferror(file))
		goto invalid;
	free(line);
	fclose(file);

	if (seen != 63U || format != CONTRACT_FORMAT ||
	    strcmp(contract->name, name) != 0 ||
	    !is_valid_channel_name(contract->name) ||
	    !is_valid_name(contract->source) ||
	    !is_valid_name(contract->destination) ||
	    strcmp(contract->source, contract->destination) == 0 ||
	    contract->max_message_size > (unsigned long long)(INT_MAX - 32)) {
		errno = EINVAL;
		return -1;
	}
	return 0;

invalid:
	free(line);
	fclose(file);
	errno = EINVAL;
	return -1;
}

static int read_generation(char *out, size_t outsz)
{
	if (read_text("/proc/sys/kernel/random/uuid", out, outsz) != 0)
		return -1;
	if (out[0] == '\0') {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int write_contract(const char *name,
		const struct channel_contract *contract)
{
	char path[PATH_MAX];
	FILE *file;
	int fd;
	int failed = 0;

	if (make_channel_path(path, sizeof(path), name, CONTRACT_FILE) != 0)
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -1;
	file = fdopen(fd, "w");
	if (file == NULL) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}

	if (fprintf(file,
		    "format=%d\n"
		    "name=%s\n"
		    "generation=%s\n"
		    "source=%s\n"
		    "destination=%s\n"
		    "max_message_size=%llu\n",
		    CONTRACT_FORMAT, contract->name, contract->generation,
		    contract->source, contract->destination,
		    contract->max_message_size) < 0)
		failed = 1;
	if (!failed && fflush(file) != 0)
		failed = 1;
	if (!failed && fsync(fd) != 0)
		failed = 1;
	if (!failed && fchmod(fd, 0444) != 0)
		failed = 1;
	if (fclose(file) != 0)
		failed = 1;
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

	snprintf(path, sizeof(path), "%s/%s", CG_ROOT, name);
	if (stat(path, &st) != 0) {
		if (errno == ENOENT)
			return 0;
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		errno = EINVAL;
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%s/cgroup.events", CG_ROOT, name);
	file = fopen(path, "r");
	if (file == NULL)
		return -1;
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

static int contract_has_active_domain(const struct channel_contract *contract,
		const char **active_name)
{
	int active = domain_populated(contract->source);

	if (active < 0)
		return -1;
	if (active) {
		*active_name = contract->source;
		return 1;
	}
	active = domain_populated(contract->destination);
	if (active < 0)
		return -1;
	if (active) {
		*active_name = contract->destination;
		return 1;
	}
	return 0;
}

static int lifetime_lock(const char *name, int exclusive, int nonblock)
{
	char path[PATH_MAX];
	int operation = exclusive ? LOCK_EX : LOCK_SH;
	int fd;

	if (nonblock)
		operation |= LOCK_NB;
	if (make_channel_path(path, sizeof(path), name, LIFETIME_LOCK) != 0)
		return -1;
	fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
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
	if (errno == EWOULDBLOCK)
		return 1;
	return -1;
}

static int channel_socket_exists(const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	if (make_channel_path(path, sizeof(path), name, CHANNEL_SOCKET) != 0)
		return 0;
	return lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int remove_if_exists(const char *path)
{
	if (unlink(path) == 0 || errno == ENOENT)
		return 0;
	return -1;
}

static int create_lock_file(const char *name, const char *suffix)
{
	char path[PATH_MAX];
	int fd;

	if (make_channel_path(path, sizeof(path), name, suffix) != 0)
		return -1;
	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		0600);
	if (fd < 0)
		return -1;
	if (close(fd) != 0)
		return -1;
	return 0;
}

static void cleanup_partial_channel(const char *name)
{
	char path[PATH_MAX];

	if (make_channel_path(path, sizeof(path), name, CONTRACT_FILE) == 0)
		unlink(path);
	if (make_channel_path(path, sizeof(path), name, LIFETIME_LOCK) == 0)
		unlink(path);
	if (make_channel_path(path, sizeof(path), name, SOURCE_LOCK) == 0)
		unlink(path);
	if (make_channel_path(path, sizeof(path), name, RECEIVER_LOCK) == 0)
		unlink(path);
	if (make_channel_path(path, sizeof(path), name, ENDPOINT_DIR) == 0)
		rmdir(path);
	if (make_channel_path(path, sizeof(path), name, "") == 0)
		rmdir(path);
}

static int create_channel(int argc, char **argv)
{
	const char *name = NULL;
	const char *source = NULL;
	const char *destination = NULL;
	unsigned long long max_message_size = 0;
	struct channel_contract contract;
	const char *active_name = NULL;
	char path[PATH_MAX];
	int topology_fd;
	int opt;

	static const struct option options[] = {
		{"name", required_argument, NULL, 'n'},
		{"source", required_argument, NULL, 's'},
		{"destination", required_argument, NULL, 'd'},
		{"max-message-size", required_argument, NULL, 'm'},
		{0, 0, 0, 0},
	};

	optind = 3;
	while ((opt = getopt_long(argc, argv, "+n:s:d:m:", options, NULL)) != -1) {
		switch (opt) {
		case 'n':
			name = optarg;
			break;
		case 's':
			source = optarg;
			break;
		case 'd':
			destination = optarg;
			break;
		case 'm':
			if (parse_positive(optarg, &max_message_size) != 0) {
				fprintf(stderr,
					"tgbsctl: ERROR - --max-message-size must be positive\n");
				return 2;
			}
			break;
		default:
			return 2;
		}
	}
	if (optind != argc || name == NULL || source == NULL ||
	    destination == NULL || !is_valid_channel_name(name) ||
	    !is_valid_name(source) || !is_valid_name(destination) ||
	    max_message_size == 0) {
		fprintf(stderr,
			"tgbsctl: ERROR - channel create requires valid --name, --source, "
			"--destination, and --max-message-size\n");
		return 2;
	}
	if (strcmp(source, "channels") == 0 ||
	    strcmp(destination, "channels") == 0) {
		fprintf(stderr,
			"tgbsctl: ERROR - the domain name 'channels' is reserved\n");
		return 2;
	}
	if (strcmp(source, destination) == 0) {
		fprintf(stderr,
			"tgbsctl: ERROR - source and destination must be different domains\n");
		return 2;
	}
	if (max_message_size > (unsigned long long)(INT_MAX - 32)) {
		fprintf(stderr,
			"tgbsctl: ERROR - --max-message-size exceeds the supported limit\n");
		return 2;
	}

	topology_fd = channel_topology_lock(1);
	if (topology_fd < 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to lock channel topology: %s\n",
			strerror(errno));
		return 1;
	}

	memset(&contract, 0, sizeof(contract));
	snprintf(contract.name, sizeof(contract.name), "%s", name);
	snprintf(contract.source, sizeof(contract.source), "%s", source);
	snprintf(contract.destination, sizeof(contract.destination), "%s", destination);
	contract.max_message_size = max_message_size;
	if (read_generation(contract.generation, sizeof(contract.generation)) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to generate channel identity: %s\n",
			strerror(errno));
		channel_topology_unlock(topology_fd);
		return 1;
	}

	int active = contract_has_active_domain(&contract, &active_name);
	if (active < 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to inspect participant cgroups: %s\n",
			strerror(errno));
		channel_topology_unlock(topology_fd);
		return 1;
	}
	if (active) {
		fprintf(stderr,
			"tgbsctl: ERROR - cannot create channel while domain %s is active\n",
			active_name);
		channel_topology_unlock(topology_fd);
		return 1;
	}

	if (make_channel_path(path, sizeof(path), name, "") != 0 ||
	    mkdir(path, 0755) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to create channel %s: %s\n",
			name, strerror(errno));
		channel_topology_unlock(topology_fd);
		return 1;
	}
	if (make_channel_path(path, sizeof(path), name, ENDPOINT_DIR) != 0 ||
	    mkdir(path, 0755) != 0)
		goto create_failed;
	if (create_lock_file(name, LIFETIME_LOCK) != 0 ||
	    create_lock_file(name, SOURCE_LOCK) != 0 ||
	    create_lock_file(name, RECEIVER_LOCK) != 0)
		goto create_failed;
	if (write_contract(name, &contract) != 0)
		goto create_failed;
	if (make_channel_path(path, sizeof(path), name, "") != 0 ||
	    chmod(path, 0555) != 0)
		goto create_failed;

	printf("tgbsctl: created channel %s (%s -> %s, max message %llu bytes)\n",
		name, source, destination, max_message_size);
	channel_topology_unlock(topology_fd);
	return 0;

create_failed:
	{
		int saved_errno = errno;

		cleanup_partial_channel(name);
		channel_topology_unlock(topology_fd);
		errno = saved_errno;
		fprintf(stderr, "tgbsctl: ERROR - unable to initialize channel %s: %s\n",
			name, strerror(errno));
		return 1;
	}
}

static int list_channels(void)
{
	DIR *dir;
	struct dirent *entry;
	int topology_fd = channel_topology_lock(0);

	if (topology_fd < 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to lock channel topology: %s\n",
			strerror(errno));
		return 1;
	}
	dir = opendir(CHANNEL_ROOT);
	if (dir == NULL) {
		fprintf(stderr, "tgbsctl: ERROR - unable to open %s: %s\n",
			CHANNEL_ROOT, strerror(errno));
		channel_topology_unlock(topology_fd);
		return 1;
	}

	printf("%-20s %-20s %-20s %-12s %-8s %-8s\n",
		"NAME", "SOURCE", "DESTINATION", "MAX_BYTES", "OPEN", "SOCKET");
	while ((entry = readdir(dir)) != NULL) {
		struct channel_contract contract;
		int open_handles;

		if (entry->d_name[0] == '.')
			continue;
		if (read_contract(entry->d_name, &contract) != 0) {
			printf("%-20s %-20s %-20s %-12s %-8s %-8s\n",
				entry->d_name, "(invalid)", "-", "-", "-", "-");
			continue;
		}
		open_handles = channel_has_open_handles(entry->d_name);
		printf("%-20s %-20s %-20s %-12llu %-8s %-8s\n",
			contract.name, contract.source, contract.destination,
			contract.max_message_size,
			open_handles > 0 ? "yes" : open_handles == 0 ? "no" : "?",
			channel_socket_exists(entry->d_name) ? "present" : "absent");
	}
	closedir(dir);
	channel_topology_unlock(topology_fd);
	return 0;
}

static int inspect_channel(const char *name)
{
	struct channel_contract contract;
	const char *active_name = NULL;
	int topology_fd;
	int open_handles;
	int active;

	if (!is_valid_channel_name(name)) {
		fprintf(stderr, "tgbsctl: ERROR - invalid channel name '%s'\n",
			name == NULL ? "(null)" : name);
		return 2;
	}
	topology_fd = channel_topology_lock(0);
	if (topology_fd < 0)
		return 1;
	if (read_contract(name, &contract) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to read channel %s: %s\n",
			name, strerror(errno));
		channel_topology_unlock(topology_fd);
		return 1;
	}
	open_handles = channel_has_open_handles(name);
	active = contract_has_active_domain(&contract, &active_name);

	printf("Name:             %s\n", contract.name);
	printf("Generation:       %s\n", contract.generation);
	printf("Source:           %s\n", contract.source);
	printf("Destination:      %s\n", contract.destination);
	printf("Max message size: %llu bytes\n", contract.max_message_size);
	printf("Open handles:     %s\n",
		open_handles > 0 ? "yes" : open_handles == 0 ? "no" : "unknown");
	printf("Channel socket:   %s\n",
		channel_socket_exists(name) ? "present" : "absent");
	if (active > 0)
		printf("Active domain:    %s\n", active_name);
	else if (active == 0)
		printf("Active domain:    (none)\n");
	else
		printf("Active domain:    (unknown)\n");

	channel_topology_unlock(topology_fd);
	return open_handles < 0 || active < 0 ? 1 : 0;
}

static int delete_channel(const char *name)
{
	struct channel_contract contract;
	const char *active_name = NULL;
	char path[PATH_MAX];
	struct stat st;
	int topology_fd;
	int lifetime_fd;
	int active;
	int rc = 1;

	if (!is_valid_channel_name(name)) {
		fprintf(stderr, "tgbsctl: ERROR - invalid channel name '%s'\n",
			name == NULL ? "(null)" : name);
		return 2;
	}
	topology_fd = channel_topology_lock(1);
	if (topology_fd < 0)
		return 1;
	if (read_contract(name, &contract) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to read channel %s: %s\n",
			name, strerror(errno));
		goto out_topology;
	}
	lifetime_fd = lifetime_lock(name, 1, 1);
	if (lifetime_fd < 0) {
		if (errno == EWOULDBLOCK)
			fprintf(stderr,
				"tgbsctl: ERROR - channel %s has open library handles\n", name);
		else
			fprintf(stderr,
				"tgbsctl: ERROR - unable to lock channel %s: %s\n",
				name, strerror(errno));
		goto out_topology;
	}
	active = contract_has_active_domain(&contract, &active_name);
	if (active < 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to inspect participant cgroups: %s\n",
			strerror(errno));
		goto out_lifetime;
	}
	if (active) {
		fprintf(stderr, "tgbsctl: ERROR - channel %s is used by active domain %s\n",
			name, active_name);
		goto out_lifetime;
	}
	if (make_channel_path(path, sizeof(path), name, "") != 0 ||
	    chmod(path, 0755) != 0)
		goto delete_error;

	if (make_channel_path(path, sizeof(path), name, CHANNEL_SOCKET) != 0)
		goto delete_error;
	if (lstat(path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			errno = EINVAL;
			goto delete_error;
		}
		if (unlink(path) != 0)
			goto delete_error;
	} else if (errno != ENOENT) {
		goto delete_error;
	}
	if (make_channel_path(path, sizeof(path), name, SOURCE_LOCK) != 0 ||
	    remove_if_exists(path) != 0)
		goto delete_error;
	if (make_channel_path(path, sizeof(path), name, RECEIVER_LOCK) != 0 ||
	    remove_if_exists(path) != 0)
		goto delete_error;
	if (make_channel_path(path, sizeof(path), name, ENDPOINT_DIR) != 0 ||
	    rmdir(path) != 0)
		goto delete_error;
	if (make_channel_path(path, sizeof(path), name, CONTRACT_FILE) != 0 ||
	    unlink(path) != 0)
		goto delete_error;
	if (make_channel_path(path, sizeof(path), name, LIFETIME_LOCK) != 0 ||
	    unlink(path) != 0)
		goto delete_error;
	close(lifetime_fd);
	lifetime_fd = -1;
	if (make_channel_path(path, sizeof(path), name, "") != 0 ||
	    rmdir(path) != 0)
		goto delete_error;

	printf("tgbsctl: deleted channel %s\n", name);
	rc = 0;
	goto out_topology;

delete_error:
	fprintf(stderr, "tgbsctl: ERROR - unable to delete channel %s: %s\n",
		name, strerror(errno));
out_lifetime:
	if (lifetime_fd >= 0)
		close(lifetime_fd);
out_topology:
	channel_topology_unlock(topology_fd);
	return rc;
}

void channel_mount_plan_close(struct channel_mount_plan *plan)
{
	size_t i;

	if (plan == NULL)
		return;
	for (i = 0; i < plan->count; i++) {
		if (plan->entries[i].channel_fd >= 0)
			close(plan->entries[i].channel_fd);
		if (plan->entries[i].endpoint_fd >= 0)
			close(plan->entries[i].endpoint_fd);
		if (plan->entries[i].source_lock_fd >= 0)
			close(plan->entries[i].source_lock_fd);
	}
	free(plan->entries);
	plan->entries = NULL;
	plan->count = 0;
}

static int open_channel_mount_path(const char *name, const char *suffix)
{
	char path[PATH_MAX];

	if (make_channel_path(path, sizeof(path), name, suffix) != 0)
		return -1;
	return (int)syscall(SYS_open_tree, AT_FDCWD, path,
		OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_NO_AUTOMOUNT |
		AT_SYMLINK_NOFOLLOW);
}

int channel_mount_plan_prepare(const char *domain,
		struct channel_mount_plan *plan)
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
		if (errno == ENOENT)
			return 0;
		return -1;
	}
	while ((entry = readdir(dir)) != NULL) {
		struct channel_contract contract;
		struct channel_mount_entry mount_entry = {
			.channel_fd = -1,
			.endpoint_fd = -1,
			.source_lock_fd = -1,
		};
		struct channel_mount_entry *new_entries;

		if (entry->d_name[0] == '.')
			continue;
		if (read_contract(entry->d_name, &contract) != 0) {
			errno = EINVAL;
			goto error;
		}
		if (strcmp(contract.source, domain) == 0)
			mount_entry.role = CHANNEL_MOUNT_SOURCE;
		else if (strcmp(contract.destination, domain) == 0)
			mount_entry.role = CHANNEL_MOUNT_DESTINATION;
		else
			continue;

		snprintf(mount_entry.name, sizeof(mount_entry.name), "%s",
			contract.name);
		mount_entry.channel_fd = open_channel_mount_path(contract.name, "");
		if (mount_entry.channel_fd < 0)
			goto entry_error;
		mount_entry.source_lock_fd = open_channel_mount_path(contract.name,
			SOURCE_LOCK);
		if (mount_entry.source_lock_fd < 0)
			goto entry_error;
		if (mount_entry.role == CHANNEL_MOUNT_DESTINATION) {
			mount_entry.endpoint_fd = open_channel_mount_path(contract.name,
				ENDPOINT_DIR);
			if (mount_entry.endpoint_fd < 0)
				goto entry_error;
		}

		new_entries = realloc(plan->entries,
			(plan->count + 1) * sizeof(*plan->entries));
		if (new_entries == NULL)
			goto entry_error;
		plan->entries = new_entries;
		plan->entries[plan->count++] = mount_entry;
		continue;

entry_error:
		saved_errno = errno;
		if (mount_entry.channel_fd >= 0)
			close(mount_entry.channel_fd);
		if (mount_entry.endpoint_fd >= 0)
			close(mount_entry.endpoint_fd);
		if (mount_entry.source_lock_fd >= 0)
			close(mount_entry.source_lock_fd);
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

int cmd_channel(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
			"tgbsctl: ERROR - channel requires create, list, inspect, or delete\n");
		return 2;
	}
	if (strcmp(argv[2], "create") == 0)
		return create_channel(argc, argv);
	if (strcmp(argv[2], "list") == 0) {
		if (argc != 3)
			return 2;
		return list_channels();
	}
	if (strcmp(argv[2], "inspect") == 0) {
		if (argc != 4)
			return 2;
		return inspect_channel(argv[3]);
	}
	if (strcmp(argv[2], "delete") == 0) {
		if (argc != 4)
			return 2;
		return delete_channel(argv[3]);
	}

	fprintf(stderr, "tgbsctl: ERROR - unknown channel command '%s'\n", argv[2]);
	return 2;
}
