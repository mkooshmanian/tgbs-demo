/* SPDX-License-Identifier: MIT
 * Persistent domain storage and private root mounts over the immutable image.
 */
#define _GNU_SOURCE
#include "tgbsctl.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

void rootfs_plan_close_layers(struct rootfs_plan *plan)
{
	if (plan->lower_fd >= 0)
		close(plan->lower_fd);
	if (plan->upper_fd >= 0)
		close(plan->upper_fd);
	if (plan->work_fd >= 0)
		close(plan->work_fd);
	if (plan->domain_fd >= 0)
		close(plan->domain_fd);
	plan->lower_fd = plan->upper_fd = plan->work_fd = plan->domain_fd = -1;
}

void rootfs_plan_close(struct rootfs_plan *plan)
{
	rootfs_plan_close_layers(plan);
	if (plan->lock_fd >= 0)
		close(plan->lock_fd);
	plan->lock_fd = -1;
}

/* Refuse symlinks for managed storage directories and lock files. */
static int open_directory_at(int parent, const char *name)
{
	if (mkdirat(parent, name, 0700) != 0 && errno != EEXIST)
		return -1;
	return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static int read_identity_at(int parent, const char *name, char identity[65])
{
	char buffer[66];
	int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	ssize_t count;
	size_t i;

	if (fd < 0)
		return -1;
	do {
		count = read(fd, buffer, sizeof(buffer));
	} while (count < 0 && errno == EINTR);
	close(fd);
	if (count < 0)
		return -1;
	if (count != 65 || buffer[64] != '\n') {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < 64; i++) {
		if (!((buffer[i] >= '0' && buffer[i] <= '9') ||
		      (buffer[i] >= 'a' && buffer[i] <= 'f'))) {
			errno = EINVAL;
			return -1;
		}
	}
	memcpy(identity, buffer, 64);
	identity[64] = '\0';
	return 0;
}

int rootfs_plan_prepare(const char *name, int ephemeral, struct rootfs_plan *plan)
{
	struct statvfs attributes;
	struct statfs filesystem;
	char base_id[65], store_id[65];
	int store = -1, overlays = -1, containers = -1, domain = -1;
	int detached;
	int result = -1, saved_errno;

	*plan = (struct rootfs_plan){-1, -1, -1, -1, -1};
	plan->lower_fd = open(ROOTFS_LOWER,
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (plan->lower_fd < 0 || fstatvfs(plan->lower_fd, &attributes) != 0)
		goto out;
	if (!(attributes.f_flag & ST_RDONLY)) {
		errno = EROFS;
		goto out;
	}
	/* OverlayFS can clone a layer from an anonymous detached mount, but not
	 * from a directory descriptor tied to the supervisor's mount namespace. */
	detached = (int)syscall(SYS_open_tree, plan->lower_fd, "",
		OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
	if (detached < 0)
		goto out;
	close(plan->lower_fd);
	plan->lower_fd = detached;
	if (ephemeral) {
		result = 0;
		goto out;
	}
	store = open(ROOTFS_STORE, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (store < 0 || fstatfs(store, &filesystem) != 0 ||
	    fstatvfs(store, &attributes) != 0)
		goto out;
	/* A missing data mount would otherwise create uppers in the host overlay. */
	if (filesystem.f_type != EXT4_SUPER_MAGIC || (attributes.f_flag & ST_RDONLY)) {
		errno = ENODEV;
		goto out;
	}
	if (read_identity_at(plan->lower_fd, "etc/tgbs-rootfs-id", base_id) != 0 ||
	    read_identity_at(store, "rootfs-id", store_id) != 0)
		goto out;
	if (strcmp(base_id, store_id) != 0) {
		errno = ESTALE;
		goto out;
	}
	overlays = open_directory_at(store, "overlays");
	if (overlays < 0)
		goto out;
	containers = open_directory_at(overlays, "containers");
	if (containers < 0)
		goto out;
	domain = open_directory_at(containers, name);
	if (domain < 0)
		goto out;
	plan->lock_fd = openat(domain, "lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (plan->lock_fd < 0 || flock(plan->lock_fd, LOCK_EX | LOCK_NB) != 0)
		goto out;
	/* Upper and work must use the same mount object, not two separate binds.
	 * Keep the domain tree alive until the child has created its overlay. */
	plan->domain_fd = (int)syscall(SYS_open_tree, domain, "",
		OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
	if (plan->domain_fd < 0)
		goto out;
	plan->upper_fd = open_directory_at(plan->domain_fd, "upper");
	plan->work_fd = open_directory_at(plan->domain_fd, "work");
	if (plan->upper_fd < 0 || plan->work_fd < 0)
		goto out;
	result = 0;
out:
	saved_errno = errno;
	if (domain >= 0) close(domain);
	if (containers >= 0) close(containers);
	if (overlays >= 0) close(overlays);
	if (store >= 0) close(store);
	if (result != 0)
		rootfs_plan_close(plan);
	errno = saved_errno;
	return result;
}

/* Linux >= 6.13 accepts layer directory descriptors. In particular, the base
 * remains usable after replacing /run, and upper paths never depend on names
 * in the host overlay. All descriptors are closed before application exec. */
static int mount_overlay(const struct rootfs_plan *plan, const char *target)
{
	int context = (int)syscall(SYS_fsopen, "overlay", FSOPEN_CLOEXEC);
	int detached = -1, result = -1, saved_errno;

	if (context < 0)
		return -1;
	if (syscall(SYS_fsconfig, context, FSCONFIG_SET_FD, "lowerdir+", NULL, plan->lower_fd) != 0 ||
	    syscall(SYS_fsconfig, context, FSCONFIG_SET_FD, "upperdir", NULL, plan->upper_fd) != 0 ||
	    syscall(SYS_fsconfig, context, FSCONFIG_SET_FD, "workdir", NULL, plan->work_fd) != 0 ||
	    syscall(SYS_fsconfig, context, FSCONFIG_CMD_CREATE, NULL, NULL, 0) != 0)
		goto out;
	detached = (int)syscall(SYS_fsmount, context, FSMOUNT_CLOEXEC, 0);
	if (detached < 0)
		goto out;
	result = (int)syscall(SYS_move_mount, detached, "", AT_FDCWD, target, MOVE_MOUNT_F_EMPTY_PATH);
out:
	saved_errno = errno;
	if (detached >= 0) close(detached);
	close(context);
	errno = saved_errno;
	return result;
}

int rootfs_mount(struct rootfs_plan *plan)
{
	const char *stage = "/run/tgbs-root";
	const char *merged = "/run/tgbs-root/merged";
	const char *old_root = "/run/tgbs-root/merged/run/tgbs-old-root";

	if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700") != 0 ||
	    mkdir(stage, 0700) != 0 || mkdir(merged, 0700) != 0)
		return -1;
	if (plan->upper_fd < 0) {
		if (mkdir("/run/tgbs-root/upper", 0700) != 0 ||
		    mkdir("/run/tgbs-root/work", 0700) != 0)
			return -1;
		plan->upper_fd = open("/run/tgbs-root/upper", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		plan->work_fd = open("/run/tgbs-root/work", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (plan->upper_fd < 0 || plan->work_fd < 0)
			return -1;
	}
	if (mount_overlay(plan, merged) != 0)
		return -1;
	/* The old-root mountpoint lives in tmpfs, never in the persistent upper.
	 * A killed startup cannot leave a directory that breaks the next startup. */
	if (mount("tmpfs", "/run/tgbs-root/merged/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") != 0 ||
	    mkdir(old_root, 0700) != 0 ||
	    syscall(SYS_pivot_root, merged, old_root) != 0 || chdir("/") != 0)
		return -1;
	if (mount("/run/tgbs-old-root/dev", "/dev", NULL, MS_BIND | MS_REC, NULL) != 0 ||
	    umount2("/run/tgbs-old-root", MNT_DETACH) != 0 ||
	    rmdir("/run/tgbs-old-root") != 0)
		return -1;
	return 0;
}
