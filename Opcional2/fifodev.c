/*=====================================================================================
    MODULE: fifoproc

    DESCRIPTION:
        Maintains a kernel fifo

    USAGE:

    CONDITIONAL COMPILATION

    COMMENTARIES

=======================================================================================
*/


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/vmalloc.h>
#include <linux/semaphore.h>

#include <asm-generic/uaccess.h>
#include <asm-generic/errno.h>

#include <linux/ftrace.h>

#include <linux/kfifo.h>
#include <linux/string.h>


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Modlist kernel module - FDI-UCM");
MODULE_AUTHOR("Daniel Pinto, Javier Bermudez");

/* Because of kfifo restrictions, BUFF_ITEMS must be a power of 2 */
#define MAX_BUFF_ITEMS (64)
#define MAX_KBUFF MAX_BUFF_ITEMS
#define MODULE_NAME "fifodev"
#define CLASS_NAME  "fifodev"

#define kfifo_gaps(fifo) (kfifo_size(fifo)-kfifo_len(fifo))

static int major_number;
static struct class *char_class = NULL;
static struct device *char_device = NULL;

DECLARE_KFIFO(buffer, char, MAX_BUFF_ITEMS);
int prod_count = 0;
int cons_count = 0;

struct semaphore mtx;
struct semaphore sem_prod;
struct semaphore sem_cons;

int nr_prod_waiting = 0;
int nr_cons_waiting = 0;

/*
 * waits in the "sem" queue until someone wakes it
 * It behaves the same as var_cond_wait, but interruptible
 * If interrupted, it does not have the mutex. Do not try to release it
 * returns 0, or -1 if interrupted
 */
int sem_wait_interruptible(struct semaphore *sem, struct semaphore *mutex, int *waiting);

/*
 * wakes a process waiting in "sem" queue
 */
void sem_signal(struct semaphore *sem, int *waiting);

/*
 * wakes all proccesses waiting in "sem" queue
 */
void sem_broadcast(struct semaphore *sem, int *waiting);

/*****************************************************************************
 *
 * Module functionality
 *
 ****************************************************************************/
static int fifodev_open(struct inode *inode, struct file *file) {

    if (down_interruptible(&mtx)) {
        trace_printk(MODULE_NAME": Interrupted in open mutex\n");
        return -EINTR;
    }
    
	if (file->f_mode & FMODE_READ) {
		cons_count++;
    
        /* If it is the only cons, all the possible prods must be waiting for it */
        if( cons_count == 1 ) {
            sem_broadcast(&sem_prod, &nr_prod_waiting);
        }

        /* If there are no prods, wait for someone to come */
        while( prod_count <= 0 ) {
            if (sem_wait_interruptible(&sem_cons, &mtx, &nr_cons_waiting)) {
                trace_printk(MODULE_NAME": Interrupted in open condvar\n");
                return -EINTR;
            }
        }
        trace_printk(MODULE_NAME": CONS registered\n");
	} else{
	    prod_count++;

        /* If it is the only prod, all the possible cons must be waiting for it */
        if( prod_count == 1 ) {
            sem_broadcast(&sem_cons, &nr_cons_waiting);
        }

         /* If there are no cons, wait for someone to come */
        while( cons_count <= 0 ) {
            if (sem_wait_interruptible(&sem_prod, &mtx, &nr_prod_waiting)) {
                trace_printk(MODULE_NAME": Interrupted in open condvar\n");
                return -EINTR;
            }
        }
        trace_printk(MODULE_NAME": PROD registered\n");
	}
    up(&mtx);

    return 0;
}


static int fifodev_release(struct inode *inode, struct file *file) {

    if (down_interruptible(&mtx)) {
        trace_printk(MODULE_NAME": Interrupted in release mutex\n");
        return -EINTR;
    }

	if ( file->f_mode & FMODE_READ ){
        trace_printk(MODULE_NAME": CONS unregistered\n");
		cons_count--;
	} else{
        trace_printk(MODULE_NAME": PROD unregistered\n");
	    prod_count--;
	}

    /* No one is using the fifo, clear its content */
    if( (cons_count == 0) && (prod_count == 0) ) {
        kfifo_reset(&buffer);
    }    
    /* As there are no cons, wake all the waiting prods to allow them realize this situation */
    else if( cons_count == 0 ) {
        sem_broadcast(&sem_prod, &nr_prod_waiting);
    }
    /* As there are no prods, wake all the waiting cons to allow them realize this situation */
    else if( prod_count == 0 ) {
        sem_broadcast(&sem_cons, &nr_cons_waiting);
    }

    up(&mtx);

    return 0;
}


static ssize_t fifodev_read(struct file *filp, char __user *buf, size_t len, loff_t *off) {
    int actual_len;
    int ret_value;

    if ((len > MAX_BUFF_ITEMS) || (len > MAX_KBUFF)) {
        trace_printk(MODULE_NAME": Too much items to read\n");
        return -ENOSPC;
    }

    if (down_interruptible(&mtx)) {
        trace_printk(MODULE_NAME": Interrupted in read mutex\n");
        return -EINTR;
    }


    while( kfifo_len(&buffer) < len && prod_count > 0 ) {
        if (sem_wait_interruptible(&sem_cons, &mtx, &nr_cons_waiting)) {
            trace_printk(MODULE_NAME": Interrupted in open condvar\n");
            return -EINTR;
        }
    }

    /* no prods and the buffer is empty */
    if( prod_count == 0 && kfifo_is_empty(&buffer) ) {
        up(&mtx);
        trace_printk(MODULE_NAME": no prods and buff is empty\n");
        return 0;
    }

    ret_value = kfifo_to_user(&buffer, buf, len, &actual_len);
    if (ret_value) {
        up(&mtx);
        trace_printk(MODULE_NAME": Could not copy to user\n");
        return ret_value;
    }
    (*off) += actual_len;

    /* Wake all prods */
    /* it is not just a "signal" because we do not know the prod write len,
       so it could leave gaps unwritten, and prods waiting to write*/
    sem_broadcast(&sem_prod, &nr_prod_waiting);

    up(&mtx);
    
    return actual_len;
}


static ssize_t fifodev_write(struct file *filp, const char __user *buf, size_t len, loff_t *off) {
    int actual_len;
    int ret_value;

    if (len > MAX_BUFF_ITEMS) {
        trace_printk(MODULE_NAME": Too much items to write\n");
        return -ENOSPC;
    }

    if (down_interruptible(&mtx)) {
        trace_printk(MODULE_NAME": Interrupted in write mutex\n");
        return -EINTR;
    }

    while( (kfifo_gaps(&buffer) < len) && (cons_count>0) ) {
        if (sem_wait_interruptible(&sem_prod, &mtx, &nr_prod_waiting)) {
            trace_printk(MODULE_NAME": Interrupted in open condvar\n");
            return -EINTR;
        }
    }

    if ( cons_count == 0 ) {
        up(&mtx);
        trace_printk(MODULE_NAME": No cons registered\n");
        return -EPIPE;
    }

    ret_value = kfifo_from_user(&buffer, buf, len, &actual_len);
    if (ret_value) {
        up(&mtx);
        trace_printk(MODULE_NAME": Could not copy to user\n");
        return ret_value;
    }
    (*off) += actual_len;
	
    sem_broadcast(&sem_cons, &nr_cons_waiting);
    
    up(&mtx);

    return actual_len;
}


/*****************************************************************************
 *
 * Module meta struct
 *
 ****************************************************************************/
static const struct file_operations fops = {
    .open = fifodev_open,
    .release = fifodev_release,
    .read = fifodev_read,
    .write = fifodev_write,
};



/*****************************************************************************
 *
 * Module init and cleanup
 *
 ****************************************************************************/
static int __init init_fifodev_module( void ) {

    /* init resources */
    INIT_KFIFO(buffer);

    /* condvar-like semaphores to sync prods and cons */
    sema_init(&sem_cons, 0);
    sema_init(&sem_prod, 0);
   
    /* mutex-like semaphore for mutual exclusion */ 
    sema_init(&mtx, 1);


    /* create module entry */
    major_number = register_chrdev(0, MODULE_NAME, &fops);
    if (major_number < 0) {
        printk(KERN_ALERT MODULE_NAME": failed to register a major number\n");
        return major_number;
    }

    char_class = class_create(THIS_MODULE, CLASS_NAME);
    if(IS_ERR(char_class)) {
        unregister_chrdev(major_number, MODULE_NAME);
        printk(KERN_ALERT MODULE_NAME": failed to register device class\n");
        return PTR_ERR(char_class);
    }

    char_device = device_create(char_class, NULL, MKDEV(major_number, 0), NULL, MODULE_NAME);
    if (IS_ERR(char_device)) {
        class_destroy(char_class);
        unregister_chrdev(major_number, MODULE_NAME);
        printk(KERN_ALERT MODULE_NAME": failed to create the device\n");
        return PTR_ERR(char_device);
    }

    trace_printk(MODULE_NAME": MODULE LOADED ==========\n");
    printk(KERN_INFO MODULE_NAME": Module loaded.\n");

    return 0;
}


static void __exit cleanup_fifodev_module( void ) {

    /* remove device entry */
    device_destroy(char_class, MKDEV(major_number, 0));
    class_unregister(char_class);
    class_destroy(char_class);
    unregister_chrdev(major_number, MODULE_NAME);

    /* as the fifo is not dinamically allocated, there is no resources to free */

    trace_printk(MODULE_NAME": MODULE UNLOADED =========\n");
    printk(KERN_INFO MODULE_NAME": Module unloaded.\n");
}

module_init( init_fifodev_module );
module_exit( cleanup_fifodev_module );



/*****************************************************************************
 *
 * Semaphore-based syncronization functions
 *
 ****************************************************************************/
int sem_wait_interruptible(struct semaphore *sem, struct semaphore *mutex, int *waiting) {

    (*waiting)++;
    
    /* release the mutex */
    up(mutex);

    /* wait to be waken */
    if (down_interruptible(sem)) {
        down(mutex);
        (*waiting)--;
        up(mutex);
        return -1;
    }

    /* get the mutex */
    if (down_interruptible(mutex)) {
        return -1;
    }

    return 0;
}


void sem_signal(struct semaphore *sem, int *waiting) {
    if (*waiting > 0) {
        up(sem);
        (*waiting)--;
    }
}


void sem_broadcast(struct semaphore *sem, int *waiting) {
    while (*waiting > 0) {
        up(sem);
        (*waiting)--;
    }
}

