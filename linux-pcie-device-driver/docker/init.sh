#!/bin/busybox sh
# PID 1 inside the QEMU guest (busybox initramfs). Mounts virtual filesystems,
# exercises pcie_edu.ko, and powers off with a clear pass/fail marker.
# Extended incrementally as milestones are completed — checks are never removed
# so later milestone runs re-verify earlier ones too.
set -e

/bin/busybox --install -s /bin

mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

# M1: confirm the edu PCI device is visible on the bus
echo "=== lspci: edu device ==="
lspci -d 1234:11e8 -v || echo "WARN: lspci unavailable or device not found"

# Snapshot /proc/iomem before insmod for the unload-safety check at the end
cat /proc/iomem > /tmp/iomem_before

echo "=== insmod pcie_edu.ko ==="
insmod /pcie_edu.ko
lsmod | grep pcie_edu
ls -l /dev/pcie_edu

echo "=== dmesg: probe output ==="
dmesg | tail -n 40

# M3: confirm MSI interrupt registered while module is loaded
echo "=== /proc/interrupts while loaded ==="
cat /proc/interrupts
if ! grep -q pcie_edu /proc/interrupts 2>/dev/null; then
    echo "FAIL: pcie_edu not in /proc/interrupts (MSI not registered)"
    poweroff -f
fi
echo "PASS: pcie_edu MSI interrupt registered"

# M2/M4/M5: open + liveness + compute + DMA round-trip
echo "=== pcie_edu_test ==="
/pcie_edu_test

echo "=== rmmod pcie_edu ==="
rmmod pcie_edu

# M6: verify no MMIO resources leaked (BAR0 must be released)
echo "=== /proc/iomem delta after rmmod (expect empty) ==="
cat /proc/iomem > /tmp/iomem_after
if ! diff /tmp/iomem_before /tmp/iomem_after; then
    echo "FAIL: /proc/iomem changed after rmmod — possible BAR resource leak"
    poweroff -f
fi
echo "PASS: /proc/iomem unchanged after rmmod"

# M6: verify no interrupt line leaked
if grep -q pcie_edu /proc/interrupts 2>/dev/null; then
    echo "FAIL: pcie_edu still in /proc/interrupts after rmmod"
    poweroff -f
fi
echo "PASS: no pcie_edu entry in /proc/interrupts after rmmod"

echo "ALL TESTS PASSED"
poweroff -f
