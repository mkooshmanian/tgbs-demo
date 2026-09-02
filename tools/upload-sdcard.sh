#!/usr/bin/env bash
set -euo pipefail

# Usage: sudo ./upload-sdcard.sh images/sdcard.img /dev/sda

IMAGE="${1:-}"
DEVICE="${2:-}"

if [[ -z "$IMAGE" || -z "$DEVICE" ]]; then
    echo "Usage: $0 <image.img> <device (e.g., /dev/sda)>"
    exit 1
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "Error: image file not found: $IMAGE"
    exit 1
fi

if [[ ! -b "$DEVICE" ]]; then
    echo "Error: block device not found: $DEVICE"
    exit 1
fi

echo "==> Unmounting all partitions on $DEVICE (if mounted)…"
sudo umount "${DEVICE}"* 2>/dev/null || true

echo "==> Writing the image to $DEVICE using dd…"
sudo dd if="$IMAGE" of="$DEVICE" bs=4M status=progress conv=fsync

echo "==> Forcing the kernel to reload the partition table…"
if ! sudo partprobe "$DEVICE" 2>/dev/null; then
    sudo partx -u "$DEVICE" 2>/dev/null || true
fi

# Handle partition suffix (/dev/sda2 vs /dev/mmcblk0p2)
if [[ "$DEVICE" =~ [0-9]$ ]]; then
    ROOT_PART="${DEVICE}p2"
else
    ROOT_PART="${DEVICE}2"
fi

echo "==> Unmounting the rootfs partition ($ROOT_PART) if mounted…"
sudo umount "$ROOT_PART" 2>/dev/null || true

echo "==> Expanding partition 2 to use the full remaining space (parted)…"
sudo parted -s "$DEVICE" resizepart 2 100%

echo "==> Reloading the partition table after resizing…"
if ! sudo partprobe "$DEVICE" 2>/dev/null; then
    sudo partx -u "$DEVICE" 2>/dev/null || true
fi

echo "==> Checking the filesystem on $ROOT_PART (e2fsck)…"
sudo e2fsck -f "$ROOT_PART"

echo "==> Resizing the filesystem to fill the partition (resize2fs)…"
sudo resize2fs "$ROOT_PART"

echo "Done."
echo "The rootfs partition ($ROOT_PART) should now use all available space on the SD card."
