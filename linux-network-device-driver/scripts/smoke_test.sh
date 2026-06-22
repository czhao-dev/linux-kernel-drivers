#!/usr/bin/env bash
#
# End-to-end smoke test: build, load, configure the test namespace, ping
# across the pair, print stats, and tear everything down. Exits non-zero
# on any failure, including a failed ping.
#
# Usage: sudo scripts/smoke_test.sh [ring_size]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RING_SIZE="${1:-}"

if [[ $EUID -ne 0 ]]; then
	echo "error: must run as root" >&2
	exit 1
fi

cleanup() {
	"$SCRIPT_DIR/teardown_netns.sh" --unload || true
}
trap cleanup EXIT

echo "==> building"
make -C "$REPO_ROOT"

echo "==> loading module and configuring namespace"
"$SCRIPT_DIR/setup_netns.sh" "$RING_SIZE"

echo "==> pinging vnet1 (ns1) from the init namespace"
if ! ping -c 4 -W 2 10.0.0.2; then
	echo "smoke test FAILED: ping did not succeed" >&2
	exit 1
fi

echo "==> driver stats"
ip -s link show vnet0
ethtool -S vnet0 || true

echo "smoke test PASSED"
