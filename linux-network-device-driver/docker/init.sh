#!/bin/sh
# PID 1 inside the QEMU guest. Builds, loads, and exercises netdrv against a
# real kernel, then reports a single PASS/FAIL marker on the console so the
# host-side runner can determine the outcome without depending on QEMU's own
# exit code (which reflects "the guest powered off cleanly", not test result).
set -u
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null

FAIL=0
fail() { echo "FAIL: $*"; FAIL=1; }
pass() { echo "ok: $*"; }

dmesg -C

setup_pair() {
	ip netns add ns1 || return 1
	ip link set vnet1 netns ns1 || return 1
	ip addr add 10.0.0.1/24 dev vnet0 || return 1
	ip link set vnet0 up || return 1
	ip netns exec ns1 ip addr add 10.0.0.2/24 dev vnet1 || return 1
	ip netns exec ns1 ip link set lo up || return 1
	ip netns exec ns1 ip link set vnet1 up || return 1
}

teardown_pair() {
	ip link set vnet0 down 2>/dev/null
	ip netns exec ns1 ip link set vnet1 down 2>/dev/null
	ip netns del ns1 2>/dev/null
}

stat_of() {
	# stat_of <dev> <netns or "-"> <ethtool-stat-name>
	dev="$1"; ns="$2"; name="$3"
	if [ "$ns" = "-" ]; then
		ethtool -S "$dev" 2>/dev/null | awk -F': *' -v n="$name" '$1 ~ n {print $2}'
	else
		ip netns exec "$ns" ethtool -S "$dev" 2>/dev/null | awk -F': *' -v n="$name" '$1 ~ n {print $2}'
	fi
}

echo "=== netdrv E2E: module load + interface registration ==="
insmod /netdrv.ko || fail "insmod failed"
ip link show vnet0 | grep -q 'vnet0' && pass "vnet0 present" || fail "vnet0 missing"
ip link show vnet1 | grep -q 'vnet1' && pass "vnet1 present" || fail "vnet1 missing"
ip link show vnet0 | grep -q 'state DOWN' && pass "vnet0 starts DOWN" || fail "vnet0 not DOWN at registration"

echo "=== netdrv E2E: carrier state on open/stop ==="
ip link set vnet0 up
sleep 0.2
ip link show vnet0 | grep -q 'LOWER_UP' && pass "carrier on after open" || fail "carrier not on after open"
ip link set vnet0 down
sleep 0.2
ip link show vnet0 | grep -q 'LOWER_UP' && fail "carrier still on after stop" || pass "carrier off after stop"

echo "=== netdrv E2E: connectivity across the pair (netns) ==="
setup_pair || fail "netns setup failed"
if ping -c 4 -W 2 10.0.0.2 >/tmp/ping1.log 2>&1; then
	pass "ping vnet0 -> vnet1 (ns1) succeeded"
else
	fail "ping vnet0 -> vnet1 (ns1) failed"
	cat /tmp/ping1.log
fi
cat /tmp/ping1.log

echo "=== netdrv E2E: stats symmetry after ping ==="
tx0=$(stat_of vnet0 - tx_packets)
rx1=$(stat_of vnet1 ns1 rx_packets)
rx0=$(stat_of vnet0 - rx_packets)
tx1=$(stat_of vnet1 ns1 tx_packets)
echo "vnet0 tx_packets=$tx0 rx_packets=$rx0 ; vnet1 tx_packets=$tx1 rx_packets=$rx1"
[ -n "$tx0" ] && [ "$tx0" = "$rx1" ] && pass "vnet0.tx_packets == vnet1.rx_packets ($tx0)" || fail "vnet0.tx_packets ($tx0) != vnet1.rx_packets ($rx1)"
[ -n "$tx1" ] && [ "$tx1" = "$rx0" ] && pass "vnet1.tx_packets == vnet0.rx_packets ($tx1)" || fail "vnet1.tx_packets ($tx1) != vnet0.rx_packets ($rx0)"

echo "=== netdrv E2E: module unload while interfaces are up ==="
teardown_pair
if rmmod netdrv 2>/tmp/rmmod1.log; then
	pass "rmmod succeeded after basic test"
else
	fail "rmmod failed after basic test"
	cat /tmp/rmmod1.log
fi
if dmesg | grep -iE 'warn|bug|oops|panic|leak'; then
	fail "dmesg shows warnings/bugs after basic load/unload cycle"
else
	pass "no warnings/bugs in dmesg after basic load/unload cycle"
fi

echo "=== netdrv E2E: backpressure with small ring (ring_size=4) ==="
dmesg -C
insmod /netdrv.ko ring_size=4 || fail "insmod ring_size=4 failed"
setup_pair || fail "netns setup failed (ring_size=4)"
if ping -f -c 2000 -W 2 10.0.0.2 >/tmp/ping2.log 2>&1; then
	pass "flood ping completed"
else
	fail "flood ping reported an error"
fi
tail -5 /tmp/ping2.log
sent=$(awk -F',' '/packets transmitted/{gsub(/[^0-9]/,"",$1); print $1}' /tmp/ping2.log)
recv=$(awk -F',' '/packets transmitted/{gsub(/[^0-9]/,"",$2); print $2}' /tmp/ping2.log)
echo "flood ping: sent=$sent recv=$recv"
[ -n "$sent" ] && [ "$sent" = "$recv" ] && pass "flood ping: no packet loss ($recv/$sent)" || fail "flood ping: packet loss observed ($recv/$sent)"

# A single-threaded ping flood round-trips fast enough that NAPI drains the
# ring before it ever builds depth (TX and the resulting softirq drain are
# tightly interleaved on the same CPU). Concurrent producers are needed to
# actually outrun NAPI and exercise netif_stop_queue: iperf3 with several
# parallel UDP streams at unlimited rate.
ip netns exec ns1 iperf3 -s -1 -D >/tmp/iperf3_server.log 2>&1
sleep 0.5
iperf3 -c 10.0.0.2 -u -b 0 -P 4 -t 5 -i 0 >/tmp/iperf3_client.log 2>&1
tail -15 /tmp/iperf3_client.log
stops=$(stat_of vnet0 - tx_queue_stops)
wakes=$(stat_of vnet0 - tx_queue_wakes)
echo "vnet0 tx_queue_stops=$stops tx_queue_wakes=$wakes"
[ -n "$stops" ] && [ "$stops" -gt 0 ] 2>/dev/null && pass "tx_queue_stops incremented ($stops) — backpressure exercised" || fail "tx_queue_stops did not increment with ring_size=4"
tx0=$(stat_of vnet0 - tx_packets)
rx1=$(stat_of vnet1 ns1 rx_packets)
echo "vnet0 tx_packets=$tx0 ; vnet1 rx_packets=$rx1"
[ -n "$tx0" ] && [ "$tx0" = "$rx1" ] && pass "no packets lost under backpressure ($tx0 == $rx1)" || fail "packet count mismatch under backpressure ($tx0 != $rx1)"

teardown_pair
rmmod netdrv 2>/tmp/rmmod2.log || fail "rmmod failed after backpressure test"
if dmesg | grep -iE 'warn|bug|oops|panic|leak'; then
	fail "dmesg shows warnings/bugs after backpressure load/unload cycle"
else
	pass "no warnings/bugs in dmesg after backpressure cycle"
fi

echo "=== netdrv E2E: repeated load/unload loop (10x) ==="
dmesg -C
loop_fail=0
i=1
while [ "$i" -le 10 ]; do
	insmod /netdrv.ko || { loop_fail=1; break; }
	ip link set vnet0 up || { loop_fail=1; break; }
	ip link set vnet1 up || { loop_fail=1; break; }
	rmmod netdrv || { loop_fail=1; break; }
	i=$((i + 1))
done
if [ "$loop_fail" -eq 0 ]; then
	pass "10x load/unload loop completed"
else
	fail "load/unload loop failed on iteration $i"
fi
if dmesg | grep -iE 'warn|bug|oops|panic|leak'; then
	fail "dmesg shows warnings/bugs after load/unload loop"
else
	pass "no warnings/bugs in dmesg after load/unload loop"
fi

echo "=============================================="
if [ "$FAIL" -eq 0 ]; then
	echo "NETDRV_E2E_RESULT: PASS"
else
	echo "NETDRV_E2E_RESULT: FAIL"
fi
echo "=============================================="

sync
reboot -f
sleep 5
poweroff -f
