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

#define CONTRACT_FILE "/contract"
#define ENDPOINT_DIR "/endpoint"
#define CHANNEL_SOCKET "/endpoint/channel.sock"
#define SOURCE_LOCK "/endpoint/source.lock"
#define RECEIVER_LOCK "/endpoint/receiver.lock"
#define LIFETIME_LOCK "/lifetime.lock"

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

static int read_contract(const char *name, struct tgbs_channel_contract *contract)
{
	return tgbs_contract_read(CHANNEL_ROOT, name, contract);
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
		const struct tgbs_channel_contract *contract)
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
		    "type=%s\n"
		    "name=%s\n"
		    "generation=%s\n"
		    "source=%s\n"
		    "max_message_size=%llu\n",
		    TGBS_CONTRACT_FORMAT, tgbs_contract_type_name(contract->type),
		    contract->name, contract->generation, contract->source,
		    contract->max_message_size) < 0)
		failed = 1;
	for (size_t i = 0; !failed && i < contract->destination_count; i++)
		if (fprintf(file, "destination=%s\n", contract->destinations[i]) < 0)
			failed = 1;
	if (!failed && contract->type == TGBS_CONTRACT_SAMPLING &&
	    fprintf(file, "refresh_period_us=%llu\n", contract->refresh_period_us) < 0)
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

static int contract_has_active_domain(const struct tgbs_channel_contract *contract,
		const char **active_name)
{
	int active = domain_populated(contract->source);

	if (active < 0)
		return -1;
	if (active) {
		*active_name = contract->source;
		return 1;
	}
	for (size_t i = 0; i < contract->destination_count; i++) {
		active = domain_populated(contract->destinations[i]);
		if (active < 0)
			return -1;
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
	struct tgbs_channel_contract contract = {0};
	const char *active_name = NULL;
	char path[PATH_MAX];
	int topology_fd;
	int opt;

	static const struct option options[] = {
		{"name", required_argument, NULL, 'n'},
		{"source", required_argument, NULL, 's'},
		{"destination", required_argument, NULL, 'd'},
		{"max-message-size", required_argument, NULL, 'm'},
		{"type", required_argument, NULL, 't'},
		{"refresh-period-us", required_argument, NULL, 'r'},
		{0, 0, 0, 0},
	};

	optind = 3;
	while ((opt = getopt_long(argc, argv, "+n:s:d:m:t:r:", options, NULL)) != -1) {
		switch (opt) {
		case 'n':
			name = optarg;
			break;
		case 's':
			source = optarg;
			break;
		case 'd':
			if (tgbs_contract_add_destination(&contract, optarg) != 0) {
				fprintf(stderr, "tgbsctl: ERROR - invalid, duplicate, or too many destinations\n");
				return 2;
			}
			break;
		case 'm':
			if (tgbs_contract_parse_positive(optarg, &contract.max_message_size) != 0) {
				fprintf(stderr, "tgbsctl: ERROR - --max-message-size must be positive\n");
				return 2;
			}
			break;
		case 't':
			if (tgbs_contract_parse_type(optarg, &contract.type) != 0) {
				fprintf(stderr, "tgbsctl: ERROR - --type must be queuing or sampling\n");
				return 2;
			}
			break;
		case 'r':
			if (tgbs_contract_parse_positive(optarg, &contract.refresh_period_us) != 0) {
				fprintf(stderr, "tgbsctl: ERROR - --refresh-period-us must be positive\n");
				return 2;
			}
			break;
		default:
			return 2;
		}
	}
	if (contract.type == 0) {
		fprintf(stderr, "tgbsctl: ERROR - --type is required (queuing or sampling)\n");
		return 2;
	}
	if (optind != argc || !is_valid_channel_name(name) ||
	    !tgbs_contract_valid_name(source, sizeof(contract.source))) {
		fprintf(stderr, "tgbsctl: ERROR - channel create requires valid --name and --source\n");
		return 2;
	}
	strcpy(contract.name, name);
	strcpy(contract.source, source);
	if (read_generation(contract.generation, sizeof(contract.generation)) != 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to generate channel identity: %s\n",
			strerror(errno));
		return 1;
	}
	if (tgbs_contract_validate(&contract) != 0) {
		fprintf(stderr,
			"tgbsctl: ERROR - invalid channel contract: require a positive supported "
			"--max-message-size and valid participants; queuing requires exactly "
			"one destination and no refresh period; sampling requires one or more "
			"destinations and a positive --refresh-period-us convertible to nanoseconds\n");
		return 2;
	}

	topology_fd = channel_topology_lock(1);
	if (topology_fd < 0) {
		fprintf(stderr, "tgbsctl: ERROR - unable to lock channel topology: %s\n",
			strerror(errno));
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
	    create_lock_file(name, SOURCE_LOCK) != 0)
		goto create_failed;
	if (contract.type == TGBS_CONTRACT_QUEUING &&
	    create_lock_file(name, RECEIVER_LOCK) != 0)
		goto create_failed;
	if (write_contract(name, &contract) != 0)
		goto create_failed;
	if (make_channel_path(path, sizeof(path), name, "") != 0 ||
	    chmod(path, 0555) != 0)
		goto create_failed;

	printf("tgbsctl: created %s channel %s (source %s, %zu destination(s), "
	       "max message %llu bytes)\n",
		tgbs_contract_type_name(contract.type), name, source,
		contract.destination_count, contract.max_message_size);
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

static void print_destinations(const struct tgbs_channel_contract *contract)
{
	for (size_t i = 0; i < contract->destination_count; i++)
		printf("%s%s", i == 0 ? "" : ",", contract->destinations[i]);
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

	printf("%-20s %-8s %-20s %-12s %-12s %-8s %-8s %s\n",
		"NAME", "TYPE", "SOURCE", "MAX_BYTES", "REFRESH_US",
		"OPEN", "SOCKET", "DESTINATIONS");
	while ((entry = readdir(dir)) != NULL) {
		struct tgbs_channel_contract contract;
		char refresh[32] = "-";
		int open_handles;

		if (entry->d_name[0] == '.')
			continue;
		if (read_contract(entry->d_name, &contract) != 0) {
			printf("%-20s (invalid)\n", entry->d_name);
			continue;
		}
		if (contract.type == TGBS_CONTRACT_SAMPLING)
			snprintf(refresh, sizeof(refresh), "%llu", contract.refresh_period_us);
		open_handles = channel_has_open_handles(entry->d_name);
		printf("%-20s %-8s %-20s %-12llu %-12s %-8s %-8s ",
			contract.name, tgbs_contract_type_name(contract.type),
			contract.source, contract.max_message_size, refresh,
			open_handles > 0 ? "yes" : open_handles == 0 ? "no" : "?",
			contract.type == TGBS_CONTRACT_QUEUING ?
				(channel_socket_exists(entry->d_name) ? "present" : "absent") : "-");
		print_destinations(&contract);
		putchar('\n');
	}
	closedir(dir);
	channel_topology_unlock(topology_fd);
	return 0;
}

static int inspect_channel(const char *name)
{
	struct tgbs_channel_contract contract;
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
	printf("Type:             %s\n", tgbs_contract_type_name(contract.type));
	printf("Generation:       %s\n", contract.generation);
	printf("Source:           %s\n", contract.source);
	printf("Destination(s):   ");
	print_destinations(&contract);
	putchar('\n');
	if (contract.type == TGBS_CONTRACT_SAMPLING)
		printf("Refresh period:   %llu us\n", contract.refresh_period_us);
	printf("Max message size: %llu bytes\n", contract.max_message_size);
	printf("Open handles:     %s\n",
		open_handles > 0 ? "yes" : open_handles == 0 ? "no" : "unknown");
	if (contract.type == TGBS_CONTRACT_QUEUING)
		printf("Channel socket:   %s\n",
			channel_socket_exists(name) ? "present" : "absent");
	else
		printf("Sampling backend: not implemented\n");
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
	struct tgbs_channel_contract contract;
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

	if (contract.type == TGBS_CONTRACT_QUEUING) {
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
		struct tgbs_channel_contract contract;
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
			mount_entry.roles |= CHANNEL_MOUNT_SOURCE;
		if (tgbs_contract_has_destination(&contract, domain))
			mount_entry.roles |= CHANNEL_MOUNT_DESTINATION;
		if (mount_entry.roles == 0)
			continue;

		snprintf(mount_entry.name, sizeof(mount_entry.name), "%s",
			contract.name);
		mount_entry.type = contract.type;
		mount_entry.channel_fd = open_channel_mount_path(contract.name, "");
		if (mount_entry.channel_fd < 0)
			goto entry_error;
		mount_entry.source_lock_fd = open_channel_mount_path(contract.name,
			SOURCE_LOCK);
		if (mount_entry.source_lock_fd < 0)
			goto entry_error;
		if ((contract.type == TGBS_CONTRACT_QUEUING &&
		     (mount_entry.roles & CHANNEL_MOUNT_DESTINATION)) ||
		    (contract.type == TGBS_CONTRACT_SAMPLING &&
		     (mount_entry.roles & CHANNEL_MOUNT_SOURCE))) {
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
