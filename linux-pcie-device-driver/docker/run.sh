#!/usr/bin/env bash
# Helper for building pcie_edu.ko and running it under a real Linux kernel
# (via QEMU + a minimal busybox initramfs) from inside a Docker container,
# since macOS can't build/load kernel modules natively.
#
# Usage: docker/run.sh {build|test|shell}
#   build  — compile pcie_edu.ko and the test program only
#   test   — build, then boot QEMU with the edu device and run the test suite
#   shell  — drop into an interactive container for debugging
set -euo pipefail

IMAGE_NAME=pcie-edu-dev
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMD="${1:-}"

build_image() {
    docker build --platform linux/amd64 -t "$IMAGE_NAME" \
        -f "$PROJECT_ROOT/docker/Dockerfile" "$PROJECT_ROOT"
}

BUILD_CMD='KVER=$(ls /lib/modules | head -n1) &&
           make clean KDIR=/lib/modules/$KVER/build &&
           make KDIR=/lib/modules/$KVER/build &&
           make -C tests clean &&
           make tests'

# Packages pcie_edu.ko + test binary + busybox into an initramfs and boots it
# with qemu-system-x86_64 -device edu. docker/init.sh runs as PID 1 and drives
# the test sequence.
RUN_CMD='KVER=$(ls /lib/modules | head -n1) &&
         rm -rf /tmp/initramfs && mkdir -p /tmp/initramfs/bin &&
         cp /usr/bin/busybox /tmp/initramfs/bin/busybox &&
         cp /usr/bin/lspci /tmp/initramfs/bin/lspci &&
         ldd /usr/bin/lspci | grep -oE "/[^ ]+" | while read lib; do
             [ -f "$lib" ] && cp --parents "$lib" /tmp/initramfs/ 2>/dev/null || true
         done &&
         cp /src/docker/init.sh /tmp/initramfs/init && chmod +x /tmp/initramfs/init &&
         cp /src/pcie_edu.ko /src/tests/pcie_edu_test /tmp/initramfs/ &&
         ( cd /tmp/initramfs && find . | cpio -o -H newc 2>/dev/null | gzip > /tmp/initramfs.cpio.gz ) &&
         qemu-system-x86_64 -M q35 -cpu max -smp 1 -m 256M -nodefaults \
             -device edu \
             -kernel /boot/vmlinuz-$KVER -initrd /tmp/initramfs.cpio.gz \
             -append "console=ttyS0 panic=-1" -serial mon:stdio -nographic -no-reboot'

case "$CMD" in
  build)
    build_image
    docker run --rm --platform linux/amd64 \
        -v "$PROJECT_ROOT":/src -w /src "$IMAGE_NAME" bash -c "$BUILD_CMD"
    ;;
  test)
    build_image
    docker run --rm --platform linux/amd64 --privileged \
        -v "$PROJECT_ROOT":/src -w /src "$IMAGE_NAME" \
        bash -c "$BUILD_CMD && $RUN_CMD"
    ;;
  shell)
    build_image
    docker run --rm --platform linux/amd64 -it --privileged \
        -v "$PROJECT_ROOT":/src -w /src "$IMAGE_NAME" bash
    ;;
  *)
    echo "usage: $0 {build|test|shell}"
    exit 1
    ;;
esac
