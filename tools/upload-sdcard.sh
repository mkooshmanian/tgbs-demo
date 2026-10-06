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

# The Zybo layout is boot (1), static rootfs (2), persistent overlays (3).
# Handle partition suffix (/dev/sda3 vs /dev/mmcblk0p3).
if [[ "$DEVICE" =~ [0-9]$ ]]; then
    STATE_PART="${DEVICE}p3"
else
    STATE_PART="${DEVICE}3"
fi

echo "==> Unmounting the persistent overlay partition ($STATE_PART) if mounted…"
sudo umount "$STATE_PART" 2>/dev/null || true

echo "==> Expanding partition 3 to use the full remaining space (parted)…"
sudo parted -s "$DEVICE" resizepart 3 100%

echo "==> Reloading the partition table after resizing…"
if ! sudo partprobe "$DEVICE" 2>/dev/null; then
    sudo partx -u "$DEVICE" 2>/dev/null || true
fi

echo "==> Checking the filesystem on $STATE_PART (e2fsck)…"
sudo e2fsck -f "$STATE_PART"

echo "==> Resizing the filesystem to fill the partition (resize2fs)…"
sudo resize2fs "$STATE_PART"

echo "Done."
echo "The persistent overlay partition ($STATE_PART) should now use all remaining space on the SD card."
