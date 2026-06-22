FROM --platform=linux/amd64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# build-essential + linux-headers-generic: compile pcie_edu.ko against real kernel headers
# linux-image-generic: provides a bootable vmlinuz to run under QEMU
# qemu-system-x86: provides qemu-system-x86_64 with -device edu support
# kmod: insmod/rmmod/lsmod inside the guest initramfs
# pciutils: lspci inside the guest (for the lspci -d 1234:11e8 check)
# busybox-static + cpio: build a minimal initramfs for the QEMU guest
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        linux-headers-generic \
        linux-image-generic \
        kmod \
        qemu-system-x86 \
        busybox-static \
        cpio \
        pciutils \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
