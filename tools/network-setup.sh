#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 <network-interface>

Example:
  $0 enp89s0
EOF
}

if [[ $# -ne 1 ]]; then
    usage >&2
    exit 1
fi

INTERFACE="$1"

nmcli dev set $INTERFACE managed no
sudo ip addr flush dev $INTERFACE
sudo ip addr add 192.168.10.1/24 dev $INTERFACE
