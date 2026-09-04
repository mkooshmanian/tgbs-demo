#!/usr/bin/env bash
set -euo pipefail

HOST_ADDRESS="${ZYBO_HOST_ADDRESS:-192.168.10.1/24}"
TARGET_ADDRESS="${ZYBO_TARGET_ADDRESS:-192.168.10.2}"
TARGET_USER="${ZYBO_TARGET_USER:-root}"

usage() {
    cat <<EOF
Usage: $0 <network-interface>

Configure a direct Ethernet connection to the Zybo Z7 and open an SSH session.

Environment variables:
  ZYBO_HOST_ADDRESS    Host address in CIDR notation (default: $HOST_ADDRESS)
  ZYBO_TARGET_ADDRESS  Zybo Z7 address (default: $TARGET_ADDRESS)
  ZYBO_TARGET_USER     SSH user (default: $TARGET_USER)

Example:
  $0 enp89s0
EOF
}

if [[ $# -ne 1 ]]; then
    usage >&2
    exit 1
fi

INTERFACE="$1"
CONNECTION_NAME="tgbs-zybo-${INTERFACE}"

for command in nmcli ssh sudo; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Error: required command not found: $command" >&2
        exit 1
    fi
done

if [[ ! -e "/sys/class/net/${INTERFACE}" ]]; then
    echo "Error: network interface not found: $INTERFACE" >&2
    echo "Available interfaces:" >&2
    nmcli --get-values DEVICE device status | sed 's/^/  /' >&2
    exit 1
fi

echo "==> Configuring $INTERFACE with $HOST_ADDRESS"

if sudo nmcli connection show "$CONNECTION_NAME" >/dev/null 2>&1; then
    sudo nmcli connection modify "$CONNECTION_NAME" \
        connection.interface-name "$INTERFACE" \
        connection.autoconnect no \
        ipv4.method manual \
        ipv4.addresses "$HOST_ADDRESS" \
        ipv4.never-default yes \
        ipv6.method disabled
else
    sudo nmcli connection add \
        type ethernet \
        ifname "$INTERFACE" \
        con-name "$CONNECTION_NAME" \
        connection.autoconnect no \
        ipv4.method manual \
        ipv4.addresses "$HOST_ADDRESS" \
        ipv4.never-default yes \
        ipv6.method disabled
fi

sudo nmcli connection up "$CONNECTION_NAME"

echo "==> Connecting to ${TARGET_USER}@${TARGET_ADDRESS}"
exec ssh \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    "${TARGET_USER}@${TARGET_ADDRESS}"
