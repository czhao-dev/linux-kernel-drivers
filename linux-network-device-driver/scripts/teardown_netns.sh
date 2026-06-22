#!/usr/bin/env bash
#
# Tear down the netdrv test namespace and, optionally, unload the module.
# Deleting ns1 moves vnet1 back to the init namespace automatically (the
# kernel's default behavior for netns-unaware virtual devices), so rmmod
# can clean it up normally afterward.
#
# Usage: sudo scripts/teardown_netns.sh [--unload]

set -euo pipefail

NS=ns1
UNLOAD=0
[[ "${1:-}" == "--unload" ]] && UNLOAD=1

if [[ $EUID -ne 0 ]]; then
	echo "error: must run as root" >&2
	exit 1
fi

if ip netns list | grep -q "^${NS}\b"; then
	echo "deleting namespace $NS"
	ip netns del "$NS"
else
	echo "namespace $NS does not exist, skipping"
fi

if [[ $UNLOAD -eq 1 ]]; then
	if lsmod | grep -q '^netdrv '; then
		echo "unloading netdrv"
		rmmod netdrv
	else
		echo "netdrv not loaded, skipping rmmod"
	fi
fi
