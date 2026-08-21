/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h> // kmalloc, kfree
#include <linux/uaccess.h> // copy_to_user, copy_from_user
#include "aesdchar.h"

int aesd_major = 0; // use dynamic major
int aesd_minor = 0;

MODULE_AUTHOR("Pravin Radhakrishnan"); 
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    
    // Store a pointer to the aesd_dev structure in private_data for easy access in read/write
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    // Nothing special needed here unless tracking open instances
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry = NULL;
    size_t entry_offset_byte = 0;
    size_t bytes_to_copy = 0;
    size_t remaining_bytes = count;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    // Find the entry and byte offset in the circular buffer corresponding to *f_pos
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer, *f_pos, &entry_offset_byte);
    if (!entry) {
        goto out;
    }

    // Determine how many bytes we can read from this entry
    bytes_to_copy = entry->size - entry_offset_byte;
    if (bytes_to_copy > remaining_bytes) {
        bytes_to_copy = remaining_bytes;
    }

    if (copy_to_user(buf, entry->buffptr + entry_offset_byte, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    const char *newline_pos = NULL;
    size_t i;
    
    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    // Allocate kernel memory to copy data from user space
    char *kern_buf = kmalloc(count, GFP_KERNEL);
    if (!kern_buf) {
        retval = -ENOMEM;
        goto out_unlock;
    }

    if (copy_from_user(kern_buf, buf, count)) {
        retval = -EFAULT;
        kfree(kern_buf);
        goto out_unlock;
    }

    // Check if there is a newline character in the incoming chunk
    for (i = 0; i < count; i++) {
        if (kern_buf[i] == '\n') {
            newline_pos = &kern_buf[i];
            break;
        }
    }

    // If we don't have an active working entry yet, initialize one
    if (dev->working_entry.size == 0) {
        dev->working_entry.buffptr = kmalloc(count, GFP_KERNEL);
        if (!dev->working_entry.buffptr) {
            kfree(kern_buf);
            retval = -ENOMEM;
            goto out_unlock;
        }
        memcpy((void *)dev->working_entry.buffptr, kern_buf, count);
        dev->working_entry.size = count;
    } else {
        // Append to existing working entry
        size_t new_size = dev->working_entry.size + count;
        char *new_ptr = krealloc(dev->working_entry.buffptr, new_size, GFP_KERNEL);
        if (!new_ptr) {
            kfree(kern_buf);
            retval = -ENOMEM;
            goto out_unlock;
        }
        dev->working_entry.buffptr = new_ptr;
        memcpy((void *)(dev->working_entry.buffptr + dev->working_entry.size), kern_buf, count);
        dev->working_entry.size = new_size;
    }

    kfree(kern_buf);

    // If a newline was found, add the completed command to the circular buffer
    if (newline_pos) {
        const char *overwritten_buffer = aesd_circular_buffer_add_entry(&dev->circular_buffer, &dev->working_entry);
        if (overwritten_buffer) {
            kfree(overwritten_buffer); // Free memory of overwritten entry if buffer was full (10 items)
        }
        // Reset working entry for next command
        dev->working_entry.buffptr = NULL;
        dev->working_entry.size = 0;
    }

    retval = count;

out_unlock:
    mutex_unlock(&dev->lock);
    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    // Initialize mutex and circular buffer
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.circular_buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    // Free all dynamically allocated buffers in the circular buffer and working entry
    if (aesd_device.working_entry.buffptr) {
        kfree(aesd_device.working_entry.buffptr);
    }

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
        }
    }

    mutex_destroy(&aesd_device.lock);
    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);