#!/bin/bash
# Boots the real, version-matched kernel + initramfs (containing netdrv.ko
# and the test sequence in init.sh) under QEMU, then determines pass/fail
# from the marker init.sh prints to the serial console — not from QEMU's
# own exit code, which only reflects "the guest shut down", not test result.
set -u
LOG=/tmp/qemu.log

timeout 180 qemu-system-aarch64 \
	-M virt -cpu max -smp 2 -m 512M \
	-kernel /vmlinuz -initrd /initramfs.cpio.gz \
	-append "console=ttyAMA0 panic=-1 quiet" \
	-nographic -no-reboot -nic none \
	2>&1 | tee "$LOG"

if grep -q "NETDRV_E2E_RESULT: PASS" "$LOG"; then
	echo "=== HOST: overall result PASS ==="
	exit 0
elif grep -q "NETDRV_E2E_RESULT: FAIL" "$LOG"; then
	echo "=== HOST: overall result FAIL ==="
	exit 1
else
	echo "=== HOST: no result marker found (boot failure, crash, or timeout) ==="
	exit 2
fi
