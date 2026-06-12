// SPDX-License-Identifier: GPL-2.0
/*
 * circbuf - virtual circular buffer character device
 *
 * Registers /dev/circbuf as a bounded pipe: writers fill a fixed-size
 * circular buffer, readers drain it in FIFO order. Blocks (by default)
 * when full/empty, respects O_NONBLOCK, and exposes buffer statistics
 * via ioctl.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/minmax.h>
#include <linux/sched.h>

#include "circbuf.h"

static unsigned long buffer_size = 4096;
module_param(buffer_size, ulong, 0444);
MODULE_PARM_DESC(buffer_size, "Size of the circular buffer in bytes (default 4096)");

struct circbuf_dev {
	char *buf;
	size_t capacity;
	size_t head;   /* next byte to read */
	size_t tail;   /* next free slot to write */
	size_t count;  /* bytes currently stored */

	struct mutex lock;
	wait_queue_head_t read_q;
	wait_queue_head_t write_q;

	unsigned long reads;
	unsigned long writes;
};

static struct circbuf_dev *circbuf_device;

static int circbuf_open(struct inode *inode, struct file *file)
{
	file->private_data = circbuf_device;
	pr_debug("circbuf: open (pid %d)\n", task_pid_nr(current));
	return 0;
}

static int circbuf_release(struct inode *inode, struct file *file)
{
	pr_debug("circbuf: release (pid %d)\n", task_pid_nr(current));
	return 0;
}

static ssize_t circbuf_read(struct file *file, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct circbuf_dev *dev = file->private_data;
	size_t to_copy, first_chunk;

	if (count == 0)
		return 0;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	while (dev->count == 0) {
		mutex_unlock(&dev->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(dev->read_q, dev->count > 0))
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;
	}

	to_copy = min(count, dev->count);
	first_chunk = min(to_copy, dev->capacity - dev->head);

	if (copy_to_user(ubuf, dev->buf + dev->head, first_chunk)) {
		mutex_unlock(&dev->lock);
		return -EFAULT;
	}
	if (to_copy > first_chunk) {
		if (copy_to_user(ubuf + first_chunk, dev->buf,
				  to_copy - first_chunk)) {
			mutex_unlock(&dev->lock);
			return -EFAULT;
		}
	}

	dev->head = (dev->head + to_copy) % dev->capacity;
	dev->count -= to_copy;
	dev->reads++;

	mutex_unlock(&dev->lock);
	wake_up_interruptible(&dev->write_q);

	return to_copy;
}

static ssize_t circbuf_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct circbuf_dev *dev = file->private_data;
	size_t to_copy, first_chunk, free_space;

	if (count == 0)
		return 0;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	while (dev->count == dev->capacity) {
		mutex_unlock(&dev->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(dev->write_q,
					      dev->count < dev->capacity))
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;
	}

	free_space = dev->capacity - dev->count;
	to_copy = min(count, free_space);
	first_chunk = min(to_copy, dev->capacity - dev->tail);

	if (copy_from_user(dev->buf + dev->tail, ubuf, first_chunk)) {
		mutex_unlock(&dev->lock);
		return -EFAULT;
	}
	if (to_copy > first_chunk) {
		if (copy_from_user(dev->buf, ubuf + first_chunk,
				    to_copy - first_chunk)) {
			mutex_unlock(&dev->lock);
			return -EFAULT;
		}
	}

	dev->tail = (dev->tail + to_copy) % dev->capacity;
	dev->count += to_copy;
	dev->writes++;

	mutex_unlock(&dev->lock);
	wake_up_interruptible(&dev->read_q);

	return to_copy;
}

static long circbuf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct circbuf_dev *dev = file->private_data;
	struct circbuf_stats stats;

	switch (cmd) {
	case CIRCBUF_GET_STATS:
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;

		stats.capacity = dev->capacity;
		stats.used = dev->count;
		stats.available = dev->capacity - dev->count;
		stats.reads = dev->reads;
		stats.writes = dev->writes;

		mutex_unlock(&dev->lock);

		if (copy_to_user((struct circbuf_stats __user *)arg, &stats,
				  sizeof(stats)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations circbuf_fops = {
	.owner          = THIS_MODULE,
	.open           = circbuf_open,
	.release        = circbuf_release,
	.read           = circbuf_read,
	.write          = circbuf_write,
	.unlocked_ioctl = circbuf_ioctl,
	.llseek         = no_llseek,
};

static struct miscdevice circbuf_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = CIRCBUF_DEVICE_NAME,
	.fops  = &circbuf_fops,
	.mode  = 0666,
};

static int __init circbuf_init(void)
{
	int ret;

	if (buffer_size == 0) {
		pr_err("circbuf: buffer_size must be greater than 0\n");
		return -EINVAL;
	}

	circbuf_device = kzalloc(sizeof(*circbuf_device), GFP_KERNEL);
	if (!circbuf_device)
		return -ENOMEM;

	circbuf_device->buf = kmalloc(buffer_size, GFP_KERNEL);
	if (!circbuf_device->buf) {
		kfree(circbuf_device);
		return -ENOMEM;
	}

	circbuf_device->capacity = buffer_size;
	mutex_init(&circbuf_device->lock);
	init_waitqueue_head(&circbuf_device->read_q);
	init_waitqueue_head(&circbuf_device->write_q);

	ret = misc_register(&circbuf_miscdev);
	if (ret) {
		kfree(circbuf_device->buf);
		kfree(circbuf_device);
		return ret;
	}

	pr_info("circbuf: loaded, buffer_size=%lu bytes, /dev/%s ready\n",
		buffer_size, CIRCBUF_DEVICE_NAME);
	return 0;
}

static void __exit circbuf_exit(void)
{
	misc_deregister(&circbuf_miscdev);
	kfree(circbuf_device->buf);
	kfree(circbuf_device);
	pr_info("circbuf: unloaded\n");
}

module_init(circbuf_init);
module_exit(circbuf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("circbuf project");
MODULE_DESCRIPTION("Virtual circular buffer character device");
