#!/usr/bin/env bash
set -euo pipefail

BUNDLE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v qemu-system-arm >/dev/null 2>&1; then
    echo "Error: qemu-system-arm is required" >&2
    exit 1
fi

for image in zImage rootfs.ext4; do
    if [[ ! -s "$BUNDLE_DIR/$image" ]]; then
        echo "Error: missing or empty image: $BUNDLE_DIR/$image" >&2
        exit 1
    fi
done

exec qemu-system-arm \
    -machine virt,highmem=off \
    -cpu cortex-a15 \
    -smp 2 \
    -m 1024 \
    -kernel "$BUNDLE_DIR/zImage" \
    -append 'root=/dev/vda rw console=ttyAMA0,115200 ip=dhcp net.ifnames=0 swiotlb=0' \
    -drive "id=disk0,file=$BUNDLE_DIR/rootfs.ext4,if=none,format=raw" \
    -device virtio-blk-device,drive=disk0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=tcp:127.0.0.1:5900-:5900 \
    -device virtio-net-device,netdev=net0,mac=52:54:00:12:35:01 \
    -nographic
