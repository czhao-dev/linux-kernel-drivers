# End-to-end test harness for netdrv.
#
# Docker Desktop containers share the host VM's kernel (on macOS, a LinuxKit
# build with no installable headers package), so a kernel module cannot be
# insmod'd directly in a plain container. This harness instead builds the
# module against a real, version-matched Ubuntu kernel and boots that exact
# kernel inside QEMU (running inside the container) so the driver actually
# loads and runs for real. See docker/init.sh for the test sequence.

ARG KVER=6.8.0-124-generic

# ---- builder: compile netdrv.ko against real, version-matched headers ----
FROM ubuntu:24.04 AS builder
ARG KVER
RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        build-essential "linux-headers-${KVER}" && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./Makefile
COPY src ./src
RUN make KDIR="/usr/src/linux-headers-${KVER}"

# ---- kernel: extract vmlinuz for the same kernel version (data only, no
#      maintainer scripts / grub postinst, which would fail in a container) ----
FROM ubuntu:24.04 AS kernel
ARG KVER
RUN apt-get update -qq && \
    apt-get download "linux-image-${KVER}" && \
    mkdir -p /extract && \
    dpkg -x linux-image-*.deb /extract && \
    cp "/extract/boot/vmlinuz-${KVER}" /vmlinuz

# ---- rootfs: minimal Alpine userspace with real iproute2/ethtool/kmod/ping ----
FROM --platform=linux/arm64 alpine:3.20 AS rootfs
RUN apk add --no-cache iproute2 iputils ethtool kmod util-linux iperf3
COPY --from=builder /src/src/netdrv.ko /netdrv.ko
COPY docker/init.sh /init
RUN chmod +x /init

# ---- initramfs: pack the rootfs into a gzipped cpio archive ----
FROM ubuntu:24.04 AS initramfs
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends cpio
COPY --from=rootfs / /rootfs
RUN cd /rootfs && find . | cpio -o -H newc 2>/dev/null | gzip -9 > /initramfs.cpio.gz

# ---- runner: QEMU boots the real kernel + initramfs and runs the test ----
FROM ubuntu:24.04 AS runner
RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        qemu-system-arm && \
    rm -rf /var/lib/apt/lists/*
COPY --from=kernel /vmlinuz /vmlinuz
COPY --from=initramfs /initramfs.cpio.gz /initramfs.cpio.gz
COPY docker/run-qemu.sh /run-qemu.sh
RUN chmod +x /run-qemu.sh
ENTRYPOINT ["/run-qemu.sh"]
