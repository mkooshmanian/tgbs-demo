#!/usr/bin/env bash
set -euo pipefail

error() {
    echo "Error: $*" >&2
    exit 1
}

[[ $# -eq 0 ]] || error "usage: $0 (no arguments)"

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEPLOY_DIR="$PROJECT_DIR/build/tmp/deploy/images"
RELEASE_DIR="$PROJECT_DIR/build/release"

for command in git sort tar; do
    command -v "$command" >/dev/null 2>&1 || error "required command not found: $command"
done

check_file() {
    [[ -f "$1" && -s "$1" ]] || error "missing or empty artifact: $1"
}

QEMU_KERNEL="$DEPLOY_DIR/qemuarm32/zImage"
QEMU_ROOTFS="$DEPLOY_DIR/qemuarm32/tgbs-demo-image-qemuarm32.rootfs.ext4"
ZYBO_SDCARD="$DEPLOY_DIR/zybo-z7/tgbs-demo-image-zybo-z7.rootfs.wic"

for file in \
    "$QEMU_KERNEL" "$QEMU_ROOTFS" "$ZYBO_SDCARD" \
    "$PROJECT_DIR/tools/run-qemu.sh" "$PROJECT_DIR/tools/ssh-qemu.sh" \
    "$PROJECT_DIR/tools/upload-sdcard.sh" "$PROJECT_DIR/tools/ssh-zybo.sh"; do
    check_file "$file"
done

# Query origin so an unpublished local tag cannot become a release version.
if ! REMOTE_TAGS="$(git -C "$PROJECT_DIR" ls-remote --tags --refs origin 'refs/tags/v*')"; then
    error "cannot list published tags on origin"
fi
VERSION="$(printf '%s\n' "$REMOTE_TAGS" | sed -nE 's/^[[:xdigit:]]+[[:space:]]+refs\/tags\/(v[0-9]+(\.[0-9]+)*)$/\1/p' | sort -V | tail -n 1)"
[[ -n "$VERSION" ]] || error "origin has no published version tag (expected v1.0, v1.1, ...)"

QEMU_NAME="tgbs-demo-qemu-$VERSION"
ZYBO_NAME="tgbs-demo-zybo-z7-$VERSION"

mkdir -p -- "$RELEASE_DIR"
for name in "$QEMU_NAME" "$ZYBO_NAME"; do
    [[ ! -e "$RELEASE_DIR/$name" && ! -e "$RELEASE_DIR/$name.tar.gz" ]] || \
        error "release already exists: $name (remove its generated files before retrying)"
done

STAGING_DIR="$(mktemp -d "$RELEASE_DIR/.stage.XXXXXXXX")"
trap 'rm -rf -- "$STAGING_DIR"' EXIT

mkdir -- "$STAGING_DIR/$QEMU_NAME" "$STAGING_DIR/$ZYBO_NAME"
cp -L -- "$QEMU_KERNEL" "$STAGING_DIR/$QEMU_NAME/zImage"
cp -L -- "$QEMU_ROOTFS" "$STAGING_DIR/$QEMU_NAME/rootfs.ext4"
install -m 0755 -- "$PROJECT_DIR/tools/run-qemu.sh" "$PROJECT_DIR/tools/ssh-qemu.sh" "$STAGING_DIR/$QEMU_NAME/"

cat > "$STAGING_DIR/$QEMU_NAME/README.md" <<'EOF'
# TGBS Demo — QEMU

Install `qemu-system-arm`, then from this directory run:

```sh
./run-qemu.sh
```

The serial console is in the terminal. Log in as `root` without a password.
In another terminal, run `./ssh-qemu.sh` for SSH. A VNC viewer can connect to
`localhost:5900` after the demo starts its VNC service.
EOF

cp -L -- "$ZYBO_SDCARD" "$STAGING_DIR/$ZYBO_NAME/sdcard.wic"
install -m 0755 -- "$PROJECT_DIR/tools/upload-sdcard.sh" "$PROJECT_DIR/tools/ssh-zybo.sh" "$STAGING_DIR/$ZYBO_NAME/"

cat > "$STAGING_DIR/$ZYBO_NAME/README.md" <<'EOF'
# TGBS Demo — Zybo Z7

From this directory, write the image to an SD card (replace `/dev/sdX` with
the whole card device; its contents will be erased):

```sh
./upload-sdcard.sh ./sdcard.wic /dev/sdX
```

Boot the Zybo Z7 from the card. For a direct Ethernet connection, use your
host's network interface name to configure the link and open SSH:

```sh
./ssh-zybo.sh enp89s0
```

Log in as `root` without a password.
EOF

for name in "$QEMU_NAME" "$ZYBO_NAME"; do
    tar -C "$STAGING_DIR" -czf "$STAGING_DIR/$name.tar.gz" "$name"
done

for name in "$QEMU_NAME" "$ZYBO_NAME"; do
    mv -- "$STAGING_DIR/$name" "$STAGING_DIR/$name.tar.gz" "$RELEASE_DIR/"
    echo "Created $RELEASE_DIR/$name.tar.gz"
done
