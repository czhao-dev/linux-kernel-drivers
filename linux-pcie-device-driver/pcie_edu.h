#ifndef PCIE_EDU_H
#define PCIE_EDU_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define PCIE_EDU_DEVICE_NAME "pcie_edu"

#define PCIE_EDU_VENDOR_ID 0x1234
#define PCIE_EDU_DEVICE_ID 0x11e8

/*
 * Register offsets and bit definitions follow docs/edu-register-map.md, which
 * records the verified QEMU edu register map in detail.
 */
#define PCIE_EDU_REG_IDENT		0x00
#define PCIE_EDU_REG_LIVENESS		0x04
#define PCIE_EDU_REG_FACTORIAL		0x08
#define PCIE_EDU_REG_STATUS		0x20
#define PCIE_EDU_REG_IRQ_STATUS		0x24
#define PCIE_EDU_REG_IRQ_ACK		0x64
#define PCIE_EDU_REG_DMA_SRC		0x80
#define PCIE_EDU_REG_DMA_DST		0x88
#define PCIE_EDU_REG_DMA_CNT		0x90
#define PCIE_EDU_REG_DMA_CMD		0x98

#define PCIE_EDU_IDENT_MAGIC_MASK	0x0000ffff
#define PCIE_EDU_IDENT_MAGIC		0x000000ed

#define PCIE_EDU_STATUS_COMPUTING	(1U << 0)
#define PCIE_EDU_STATUS_IRQFACT		(1U << 7)  /* must be set by driver to enable factorial IRQ */

#define PCIE_EDU_IRQ_FACT		(1U << 0)
#define PCIE_EDU_IRQ_DMA		(1U << 8)

#define PCIE_EDU_DMA_RUN		(1U << 0)
#define PCIE_EDU_DMA_DIR		(1U << 1)  /* 0=host→device, 1=device→host */
#define PCIE_EDU_DMA_IRQ_EN		(1U << 2)

#define PCIE_EDU_DMA_BUF_SIZE		4096
#define PCIE_EDU_DMA_DEVBUF		0x40000ULL  /* device-internal DMA buffer address */

struct pcie_edu_dma_xfer {
	__u32 len;
	__u8  pattern;
};

#define PCIE_EDU_MAGIC			0xE0
#define PCIE_EDU_COMPUTE		_IOWR(PCIE_EDU_MAGIC, 1, __u32)
#define PCIE_EDU_DMA_TRANSFER		_IOWR(PCIE_EDU_MAGIC, 2, struct pcie_edu_dma_xfer)

#endif /* PCIE_EDU_H */
