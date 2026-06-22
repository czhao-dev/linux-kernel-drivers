// SPDX-License-Identifier: GPL-2.0
/*
 * pcie_edu - Linux PCI driver for QEMU's edu device
 *
 * Implements the full driver scope from the README:
 *   M1/M2: PCI probe/remove, BAR0 MMIO, /dev/pcie_edu char device,
 *           liveness register read/write
 *   M3:    MSI interrupt wiring, IRQ handler
 *   M4:    Blocking PCIE_EDU_COMPUTE ioctl (factorial via interrupt)
 *   M5:    DMA-coherent buffer, PCIE_EDU_DMA_TRANSFER ioctl (round-trip)
 *   M6:    Strict reverse-order remove(), goto-chain probe() error paths
 */

#include <linux/container_of.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "pcie_edu.h"

#define PCIE_EDU_BAR 0

struct pcie_edu_dev {
	struct pci_dev		*pdev;
	void __iomem		*mmio;
	void			*dma_buf;
	dma_addr_t		 dma_handle;
	struct miscdevice	 misc;
	struct mutex		 lock;
	wait_queue_head_t	 wait_queue;
	bool			 result_ready;
	bool			 dma_done;
};

/* Write a 64-bit value to an MMIO register as two 32-bit writes (lo then hi). */
static inline void edu_write64(void __iomem *mmio, u32 off, u64 val)
{
	iowrite32((u32)(val & 0xffffffffU), mmio + off);
	iowrite32((u32)(val >> 32), mmio + off + 4);
}

static irqreturn_t pcie_edu_irq(int irq, void *data)
{
	struct pcie_edu_dev *edu = data;
	u32 status;

	status = ioread32(edu->mmio + PCIE_EDU_REG_IRQ_STATUS);
	if (!status)
		return IRQ_NONE;

	if (status & PCIE_EDU_IRQ_FACT) {
		iowrite32(PCIE_EDU_IRQ_FACT, edu->mmio + PCIE_EDU_REG_IRQ_ACK);
		edu->result_ready = true;
		wake_up_interruptible(&edu->wait_queue);
	}

	if (status & PCIE_EDU_IRQ_DMA) {
		iowrite32(PCIE_EDU_IRQ_DMA, edu->mmio + PCIE_EDU_REG_IRQ_ACK);
		edu->dma_done = true;
		wake_up_interruptible(&edu->wait_queue);
	}

	return IRQ_HANDLED;
}

static int pcie_edu_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct pcie_edu_dev *edu = container_of(misc, struct pcie_edu_dev, misc);

	file->private_data = edu;
	dev_dbg(&edu->pdev->dev, "opened /dev/%s\n", PCIE_EDU_DEVICE_NAME);
	return 0;
}

static int pcie_edu_release(struct inode *inode, struct file *file)
{
	struct pcie_edu_dev *edu = file->private_data;

	dev_dbg(&edu->pdev->dev, "released /dev/%s\n", PCIE_EDU_DEVICE_NAME);
	return 0;
}

static ssize_t pcie_edu_read(struct file *file, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct pcie_edu_dev *edu = file->private_data;
	u32 value;

	if (count < sizeof(value))
		return -EINVAL;

	if (mutex_lock_interruptible(&edu->lock))
		return -ERESTARTSYS;

	value = ioread32(edu->mmio + PCIE_EDU_REG_LIVENESS);

	mutex_unlock(&edu->lock);

	if (copy_to_user(ubuf, &value, sizeof(value)))
		return -EFAULT;

	return sizeof(value);
}

static ssize_t pcie_edu_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct pcie_edu_dev *edu = file->private_data;
	u32 value;

	if (count != sizeof(value))
		return -EINVAL;

	if (copy_from_user(&value, ubuf, sizeof(value)))
		return -EFAULT;

	if (mutex_lock_interruptible(&edu->lock))
		return -ERESTARTSYS;

	iowrite32(value, edu->mmio + PCIE_EDU_REG_LIVENESS);

	mutex_unlock(&edu->lock);

	return sizeof(value);
}

static long pcie_edu_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct pcie_edu_dev *edu = file->private_data;

	switch (cmd) {
	case PCIE_EDU_COMPUTE: {
		u32 n, result;

		if (copy_from_user(&n, (void __user *)arg, sizeof(n)))
			return -EFAULT;

		if (mutex_lock_interruptible(&edu->lock))
			return -ERESTARTSYS;

		edu->result_ready = false;
		iowrite32(n, edu->mmio + PCIE_EDU_REG_FACTORIAL);

		if (wait_event_interruptible(edu->wait_queue, edu->result_ready)) {
			mutex_unlock(&edu->lock);
			return -ERESTARTSYS;
		}

		result = ioread32(edu->mmio + PCIE_EDU_REG_FACTORIAL);
		mutex_unlock(&edu->lock);

		if (copy_to_user((void __user *)arg, &result, sizeof(result)))
			return -EFAULT;
		return 0;
	}

	case PCIE_EDU_DMA_TRANSFER: {
		struct pcie_edu_dma_xfer xfer;
		int ret = 0;

		if (copy_from_user(&xfer, (void __user *)arg, sizeof(xfer)))
			return -EFAULT;
		if (xfer.len == 0 || xfer.len > PCIE_EDU_DMA_BUF_SIZE)
			return -EINVAL;

		if (mutex_lock_interruptible(&edu->lock))
			return -ERESTARTSYS;

		memset(edu->dma_buf, xfer.pattern, xfer.len);

		/* host → device */
		edu_write64(edu->mmio, PCIE_EDU_REG_DMA_SRC, edu->dma_handle);
		edu_write64(edu->mmio, PCIE_EDU_REG_DMA_DST, PCIE_EDU_DMA_DEVBUF);
		edu_write64(edu->mmio, PCIE_EDU_REG_DMA_CNT, xfer.len);
		edu->dma_done = false;
		iowrite32(PCIE_EDU_DMA_RUN | PCIE_EDU_DMA_IRQ_EN,
			  edu->mmio + PCIE_EDU_REG_DMA_CMD);

		if (wait_event_interruptible(edu->wait_queue, edu->dma_done)) {
			mutex_unlock(&edu->lock);
			return -ERESTARTSYS;
		}

		/* invert buffer so a stale pass can't mask a broken return leg */
		memset(edu->dma_buf, (~xfer.pattern) & 0xff, xfer.len);

		/* device → host */
		edu_write64(edu->mmio, PCIE_EDU_REG_DMA_SRC, PCIE_EDU_DMA_DEVBUF);
		edu_write64(edu->mmio, PCIE_EDU_REG_DMA_DST, edu->dma_handle);
		edu_write64(edu->mmio, PCIE_EDU_REG_DMA_CNT, xfer.len);
		edu->dma_done = false;
		iowrite32(PCIE_EDU_DMA_RUN | PCIE_EDU_DMA_DIR | PCIE_EDU_DMA_IRQ_EN,
			  edu->mmio + PCIE_EDU_REG_DMA_CMD);

		if (wait_event_interruptible(edu->wait_queue, edu->dma_done)) {
			mutex_unlock(&edu->lock);
			return -ERESTARTSYS;
		}

		if (memchr_inv(edu->dma_buf, xfer.pattern, xfer.len)) {
			dev_err(&edu->pdev->dev,
				"DMA round-trip mismatch (pattern=0x%02x len=%u)\n",
				xfer.pattern, xfer.len);
			ret = -EIO;
		}

		mutex_unlock(&edu->lock);
		return ret;
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations pcie_edu_fops = {
	.owner		 = THIS_MODULE,
	.open		 = pcie_edu_open,
	.release	 = pcie_edu_release,
	.read		 = pcie_edu_read,
	.write		 = pcie_edu_write,
	.unlocked_ioctl	 = pcie_edu_ioctl,
	.llseek		 = no_llseek,
};

static int pcie_edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct pcie_edu_dev *edu;
	resource_size_t bar_start, bar_len;
	u32 ident;
	int ret;

	edu = kzalloc(sizeof(*edu), GFP_KERNEL);
	if (!edu)
		return -ENOMEM;

	edu->pdev = pdev;
	mutex_init(&edu->lock);
	init_waitqueue_head(&edu->wait_queue);
	pci_set_drvdata(pdev, edu);

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable PCI device: %d\n", ret);
		goto err_free;
	}

	bar_start = pci_resource_start(pdev, PCIE_EDU_BAR);
	bar_len   = pci_resource_len(pdev, PCIE_EDU_BAR);
	dev_info(&pdev->dev, "BAR0 start=%pa len=0x%llx\n", &bar_start,
		 (unsigned long long)bar_len);

	ret = pci_request_regions(pdev, PCIE_EDU_DEVICE_NAME);
	if (ret) {
		dev_err(&pdev->dev, "failed to request PCI regions: %d\n", ret);
		goto err_disable;
	}

	edu->mmio = pci_iomap(pdev, PCIE_EDU_BAR, 0);
	if (!edu->mmio) {
		dev_err(&pdev->dev, "failed to map BAR0\n");
		ret = -ENOMEM;
		goto err_release_regions;
	}

	pci_set_master(pdev);

	ident = ioread32(edu->mmio + PCIE_EDU_REG_IDENT);
	if ((ident & PCIE_EDU_IDENT_MAGIC_MASK) != PCIE_EDU_IDENT_MAGIC)
		dev_warn(&pdev->dev, "unexpected ident: 0x%08x\n", ident);
	else
		dev_info(&pdev->dev, "ident: 0x%08x\n", ident);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
	if (ret) {
		dev_err(&pdev->dev, "failed to set 28-bit DMA mask: %d\n", ret);
		goto err_iounmap;
	}

	edu->dma_buf = dma_alloc_coherent(&pdev->dev, PCIE_EDU_DMA_BUF_SIZE,
					  &edu->dma_handle, GFP_KERNEL);
	if (!edu->dma_buf) {
		dev_err(&pdev->dev, "failed to allocate DMA buffer\n");
		ret = -ENOMEM;
		goto err_iounmap;
	}
	dev_info(&pdev->dev, "DMA buffer: cpu=%p bus=%pad\n",
		 edu->dma_buf, &edu->dma_handle);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to allocate MSI vector: %d\n", ret);
		goto err_free_dma;
	}

	ret = request_irq(pci_irq_vector(pdev, 0), pcie_edu_irq, 0,
			  PCIE_EDU_DEVICE_NAME, edu);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ: %d\n", ret);
		goto err_free_vectors;
	}

	/* Enable factorial-done interrupt (device resets this bit to 0) */
	iowrite32(PCIE_EDU_STATUS_IRQFACT, edu->mmio + PCIE_EDU_REG_STATUS);

	edu->misc.minor  = MISC_DYNAMIC_MINOR;
	edu->misc.name   = PCIE_EDU_DEVICE_NAME;
	edu->misc.fops   = &pcie_edu_fops;
	edu->misc.parent = &pdev->dev;
	edu->misc.mode   = 0666;

	ret = misc_register(&edu->misc);
	if (ret) {
		dev_err(&pdev->dev, "failed to register misc device: %d\n", ret);
		goto err_free_irq;
	}

	dev_info(&pdev->dev, "/dev/%s ready (irq=%d dma=%pad)\n",
		 PCIE_EDU_DEVICE_NAME, pci_irq_vector(pdev, 0), &edu->dma_handle);
	return 0;

err_free_irq:
	free_irq(pci_irq_vector(pdev, 0), edu);
err_free_vectors:
	pci_free_irq_vectors(pdev);
err_free_dma:
	dma_free_coherent(&pdev->dev, PCIE_EDU_DMA_BUF_SIZE,
			  edu->dma_buf, edu->dma_handle);
err_iounmap:
	pci_clear_master(pdev);
	pci_iounmap(pdev, edu->mmio);
err_release_regions:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
err_free:
	pci_set_drvdata(pdev, NULL);
	kfree(edu);
	return ret;
}

static void pcie_edu_remove(struct pci_dev *pdev)
{
	struct pcie_edu_dev *edu = pci_get_drvdata(pdev);

	misc_deregister(&edu->misc);
	/* Disable factorial IRQ before releasing the IRQ line */
	iowrite32(0, edu->mmio + PCIE_EDU_REG_STATUS);
	free_irq(pci_irq_vector(pdev, 0), edu);
	pci_free_irq_vectors(pdev);
	dma_free_coherent(&pdev->dev, PCIE_EDU_DMA_BUF_SIZE,
			  edu->dma_buf, edu->dma_handle);
	pci_clear_master(pdev);
	pci_iounmap(pdev, edu->mmio);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	kfree(edu);

	dev_info(&pdev->dev, "removed\n");
}

static const struct pci_device_id pcie_edu_ids[] = {
	{ PCI_DEVICE(PCIE_EDU_VENDOR_ID, PCIE_EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, pcie_edu_ids);

static struct pci_driver pcie_edu_driver = {
	.name     = PCIE_EDU_DEVICE_NAME,
	.id_table = pcie_edu_ids,
	.probe    = pcie_edu_probe,
	.remove   = pcie_edu_remove,
};

static int __init pcie_edu_init(void)
{
	return pci_register_driver(&pcie_edu_driver);
}

static void __exit pcie_edu_exit(void)
{
	pci_unregister_driver(&pcie_edu_driver);
}

module_init(pcie_edu_init);
module_exit(pcie_edu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("pcie_edu project");
MODULE_DESCRIPTION("Linux PCI driver for QEMU edu device");
