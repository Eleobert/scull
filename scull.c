#include <asm-generic/errno-base.h>
#include <asm-generic/fcntl.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/semaphore.h>

#include "scull.h"

MODULE_LICENSE("Dual BSD/GPL");

static int scull_major   = 0;
static int scull_minor   = 0;
static int scull_nr_devs = 1;
static int scull_quantum = SCULL_QUANTUM;
static int scull_qset    = SCULL_QSET;

module_param(scull_major  , int, S_IRUGO);
module_param(scull_minor  , int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);

struct scull_dev 
{
    struct scull_qset *data; /* Pointer to first quantum set */
    int quantum;             /* the current quantum size */
    int qset;                /* the current array size */
    unsigned long size;      /* amount of data stored here */
    unsigned int access_key; /* used by sculluid and scullpriv */
    struct mutex lock;    /* mutual exclusion semaphore */
    struct cdev cdev;        /* Char device structure */
};

struct scull_dev* scull_devices;

void scull_cleanup_module(void)
{
    dev_t dev = MKDEV(scull_major, scull_minor);
    unregister_chrdev_region(dev, scull_nr_devs);
}


int scull_trim(struct scull_dev* dev)
{
    struct scull_qset *next, *dptr;
    int qset = dev->qset;
    int i;

    for(dptr = dev->data; dptr; dptr = next)
    {
        if(dptr->data)
        {
            for(i = 0; i < qset; i++)
                kfree(dptr->data[i]);
            kfree(dptr->data);
            dptr->data = NULL;
        }
        next = dptr->next;
        kfree(dptr);
    }

    dev->size = 0;
    dev->quantum = scull_quantum;
    dev->qset = scull_qset;
    dev->data = NULL;
    
    return 0;
}

int scull_open(struct inode* node, struct file* filp)
{
    struct scull_dev* dev = container_of(node->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev;

    if( (filp->f_flags & O_ACCMODE) == O_WRONLY)
    {
        scull_trim(dev);
    }
    return 0;
}


struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
	struct scull_qset *qs = dev->data;

        /* Allocate first qset explicitly if need be */
	if (! qs) {
		qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if (qs == NULL)
			return NULL;  /* Never mind */
		memset(qs, 0, sizeof(struct scull_qset));
	}

	/* Then follow the list */
	while (n--) {
		if (!qs->next) {
			qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if (qs->next == NULL)
				return NULL;  /* Never mind */
			memset(qs->next, 0, sizeof(struct scull_qset));
		}
		qs = qs->next;
		continue;
	}
	return qs;
}


ssize_t scull_read(struct file *filp, char __user* buf, size_t count,
                loff_t* f_pos)
{
    struct scull_dev * dev  = filp->private_data;
    struct scull_qset* dptr;

    int quantum  = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;

    int item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if(mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;
    if(*f_pos >= dev->size)
        goto out;
    if(*f_pos + count > dev->size)
        count = dev->size - *f_pos;

    item  = (long) *f_pos / itemsize;
    rest  = (long) *f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;
    dptr  = scull_follow(dev, item);

    if(dptr == NULL || !dptr->data || !dptr->data[s_pos])
        goto out; // do not fill holes

    if(count > quantum - q_pos)
        count = quantum - q_pos;
    
    if(copy_to_user(buf, dptr->data[s_pos] + q_pos, count))
    {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval  = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}


ssize_t scull_write(struct file *filp, const char __user* buf, size_t count,
                loff_t* f_pos)
{
    struct scull_dev* dev = filp->private_data;
    struct scull_qset* dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;

    if(mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    item  = (long)*f_pos / itemsize;
    rest  = (long)*f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    dptr = scull_follow(dev, item);

    if(dptr == NULL)
        goto out;
    if(!dptr->data)
    {
        dptr->data = kmalloc(qset * sizeof(char*), GFP_KERNEL);
        if(!dptr->data)
            goto out;
        memset(dptr->data, 0, qset * sizeof(char*));
    }

    if(!dptr->data[s_pos])
    {
        dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
        if(!dptr->data[s_pos])
            goto out;
    }

    if(count > quantum - q_pos)
        count = quantum - q_pos;
    
    if(copy_from_user(dptr->data[s_pos + q_pos], buf, count))
    {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval  = count;

    if(dev->size < *f_pos)
        dev->size = *f_pos;
    
out:
    mutex_unlock(&dev->lock);
    return retval;
}


struct file_operations scull_fops = 
{
    .owner   = THIS_MODULE,
    // .llseek  = scull_llseek,
    .read    = scull_read,
    .write   = scull_write,
    // .ioctl   = scull_ioctl,
    .open    = scull_open,
    // .release = scull_release,
};


static void scull_setup_cdev(struct scull_dev* dev, int index)
{
    int devno = MKDEV(scull_major, scull_minor + index);
    int err;
    
    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops   = &scull_fops;

    err = cdev_add(&dev->cdev, devno, 1);

    if(err)
        printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}


static int scull_init(void)
{
    dev_t dev = 0;
    int result = 0;
    int i = 0;

    if(scull_major)
    {
       dev = MKDEV(scull_major, scull_minor);     
       result = register_chrdev_region(dev, scull_nr_devs, "scull");
    }
    else
    {
        result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, "scull");
        scull_major = MAJOR(dev);
    }
    
    if(result < 0)
    {
        printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
        return result;
    }    

    scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);

    if(!scull_devices)
    {
        result = -ENOMEM;
        goto fail;
    }
    
    memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));

    for(; i < scull_nr_devs; i++)
    {
        //scull_devices[i].quantum = scull_quantum;
        //scull_devices[i].quantum = scull_quantum;
        mutex_init(&scull_devices[i].lock);
        scull_setup_cdev(&scull_devices[i], i);
    }
    printk("scull: device initialized");
    return 0;

fail:
    scull_cleanup_module();
    return result;
}


static void scull_exit(void)
{
    scull_cleanup_module();
}

module_init(scull_init);
module_exit(scull_exit);

