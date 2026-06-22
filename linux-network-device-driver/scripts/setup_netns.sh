#!/usr/bin/env bash
#
# Bring up the netdrv vnet0 <-> vnet1 pair for interactive testing: load
# the module if it isn't already loaded, move vnet1 into its own network
# namespace (ns1), assign addresses, and bring both links up.
#
# Usage: sudo scripts/setup_netns.sh [ring_size]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KO_PATH="$REPO_ROOT/src/netdrv.ko"
RING_SIZE="${1:-}"

NS=ns1
DEV0=vnet0
DEV1=vnet1
DEV0_ADDR=10.0.0.1/24
DEV1_ADDR=10.0.0.2/24

if [[ $EUID -ne 0 ]]; then
	echo "error: must run as root (insmod/ip netns require it)" >&2
	exit 1
fi

if lsmod | grep -q '^netdrv '; then
	echo "netdrv already loaded, skipping insmod"
else
	if [[ ! -f "$KO_PATH" ]]; then
		echo "error: $KO_PATH not found — run 'make' first" >&2
		exit 1
	fi
	echo "loading netdrv.ko${RING_SIZE:+ (ring_size=$RING_SIZE)}"
	if [[ -n "$RING_SIZE" ]]; then
		insmod "$KO_PATH" ring_size="$RING_SIZE"
	else
		insmod "$KO_PATH"
	fi
fi

if ip netns list | grep -q "^${NS}\b"; then
	echo "namespace $NS already exists, skipping creation"
else
	echo "creating network namespace $NS"
	ip netns add "$NS"
fi

if ip link show "$DEV1" >/dev/null 2>&1; then
	echo "moving $DEV1 into $NS"
	ip link set "$DEV1" netns "$NS"
elif ip netns exec "$NS" ip link show "$DEV1" >/dev/null 2>&1; then
	echo "$DEV1 already in $NS, skipping move"
else
	echo "error: $DEV1 not found in the init namespace or $NS" >&2
	exit 1
fi

ip addr replace "$DEV0_ADDR" dev "$DEV0"
ip link set "$DEV0" up

ip netns exec "$NS" ip addr replace "$DEV1_ADDR" dev "$DEV1"
ip netns exec "$NS" ip link set "$DEV1" up

echo "done: $DEV0 (init netns, ${DEV0_ADDR%/*}) <-> $DEV1 ($NS, ${DEV1_ADDR%/*})"
