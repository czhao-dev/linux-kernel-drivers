#!/bin/busybox sh
# PID 1 inside the QEMU guest (busybox initramfs). Mounts virtual filesystems,
# exercises circbuf.ko, and powers off the VM with a clear pass/fail marker.
set -e

/bin/busybox --install -s /bin

mkdir -p /proc /sys /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

echo "== insmod (default buffer_size) =="
insmod /circbuf.ko
lsmod | grep circbuf
ls -l /dev/circbuf

echo "== basic_test =="
/basic_test /dev/circbuf

echo "== query_stats =="
/query_stats /dev/circbuf

echo "== stress =="
/stress /dev/circbuf --writers=4 --readers=4 --duration=5

echo "== rmmod =="
rmmod circbuf

echo "== reload with buffer_size=16384 =="
insmod /circbuf.ko buffer_size=16384
/query_stats /dev/circbuf
rmmod circbuf

echo "ALL TESTS PASSED"

poweroff -f
