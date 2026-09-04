#!/usr/bin/env bash
set -euo pipefail

QEMU_ADDRESS="${QEMU_SSH_ADDRESS:-localhost}"
QEMU_PORT="${QEMU_SSH_PORT:-2222}"
QEMU_USER="${QEMU_SSH_USER:-root}"

if ! command -v ssh >/dev/null 2>&1; then
    echo "Error: required command not found: ssh" >&2
    exit 1
fi

exec ssh \
    -p "$QEMU_PORT" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    "${QEMU_USER}@${QEMU_ADDRESS}"
