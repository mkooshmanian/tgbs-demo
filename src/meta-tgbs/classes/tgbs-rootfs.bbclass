# SPDX-License-Identifier: MIT
# Identify the static image by its paths, contents, ownership and permissions.

IMAGE_FEATURES += "read-only-rootfs"
IMAGE_INSTALL:append = " tgbs-runtime-init"
ROOTFS_POSTPROCESS_COMMAND:append = " tgbs_rootfs_id;"

python tgbs_rootfs_id() {
    import hashlib
    import os
    import stat

    root = d.getVar("IMAGE_ROOTFS")
    identity = "etc/tgbs-rootfs-id"
    digest = hashlib.sha256()
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        for name in sorted(dirs + files):
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root)
            if relative == identity:
                continue
            info = os.lstat(path)
            digest.update(repr((relative, info.st_mode, info.st_uid, info.st_gid)).encode() + b"\0")
            if stat.S_ISLNK(info.st_mode):
                digest.update(os.fsencode(os.readlink(path)) + b"\0")
            elif stat.S_ISREG(info.st_mode):
                content = hashlib.sha256()
                with open(path, "rb") as source:
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        content.update(block)
                digest.update(content.digest())
            elif not stat.S_ISDIR(info.st_mode):
                digest.update(str(info.st_rdev).encode() + b"\0")

    etc = os.path.join(root, "etc")
    etc_info = os.stat(etc)
    with open(os.path.join(root, identity), "w") as output:
        output.write(digest.hexdigest() + "\n")
    os.chmod(os.path.join(root, identity), 0o444)
    epoch = int(d.getVar("SOURCE_DATE_EPOCH"))
    os.utime(os.path.join(root, identity), (epoch, epoch))
    os.utime(etc, ns=(etc_info.st_atime_ns, etc_info.st_mtime_ns))
}
