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
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/semaphore.h>

#include <asm-generic/uaccess.h>
#include <asm-generic/errno.h>

#include <linux/ftrace.h>

#include <linux/string.h>
#include "cbuffer.h"


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Modlist kernel module - FDI-UCM");
MODULE_AUTHOR("Daniel Pinto, Javier Bermudez");


#define MAX_BUFF_ITEMS 50
#define MAX_KBUFF MAX_BUFF_ITEMS
#define MODULE_NAME "fifoproc"

static struct proc_dir_entry *proc_entry;

cbuffer_t *buffer;
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
static int fifoproc_open(struct inode *inode, struct file *file) {

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


static int fifoproc_release(struct inode *inode, struct file *file) {

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
        clear_cbuffer_t(buffer);
    }    
    /* As there are no cons, wake all the waiting prods to allow them realize this situation */
    else if( cons_count == 0 ) {
        sem_broadcast(&sem_prod, &nr_prod_waiting);;
    }
    /* As there are no prods, wake all the waiting cons to allow them realize this situation */
    else if( prod_count == 0 ) {
        sem_broadcast(&sem_cons, &nr_cons_waiting);
    }

    up(&mtx);

    return 0;
}


static ssize_t fifoproc_read(struct file *filp, char __user *buf, size_t len, loff_t *off) {
    char kbuffer[MAX_KBUFF];
    int actual_len;

/*
    if ((*off) > 0) {
        trace_printk(MODULE_NAME": Can not read with offset\n");
        return 0;
    }
*/
    if ((len > MAX_BUFF_ITEMS) || (len > MAX_KBUFF)) {
        trace_printk(MODULE_NAME": Too much items to read\n");
        return -ENOSPC;
    }

    if (down_interruptible(&mtx)) {
        trace_printk(MODULE_NAME": Interrupted in read mutex\n");
        return -EINTR;
    }


    while( size_cbuffer_t(buffer) < len && prod_count > 0 ) {
        if (sem_wait_interruptible(&sem_cons, &mtx, &nr_cons_waiting)) {
            trace_printk(MODULE_NAME": Interrupted in open condvar\n");
            return -EINTR;
        }
    }

    /* no prods and the buffer is empty */
    if( prod_count == 0 && is_empty_cbuffer_t(buffer) ) {
        up(&mtx);
        trace_printk(MODULE_NAME": no prods and buff is empty\n");
        return 0;
    }


    actual_len = (len <= size_cbuffer_t(buffer))? len : size_cbuffer_t(buffer);
    remove_items_cbuffer_t(buffer, kbuffer, actual_len);

    // Despertar a posibles productores bloqueados
    sem_signal(&sem_prod, &nr_prod_waiting);

    // Liberar el MUTEX
    up(&mtx);

    if (copy_to_user(buf, kbuffer, actual_len)) {
        trace_printk(MODULE_NAME": Could not copy to user\n");
        return -EINVAL;
    }

    (*off) += actual_len;
    
    return actual_len;
}


static ssize_t fifoproc_write(struct file *filp, const char __user *buf, size_t len, loff_t *off) {
    char kbuffer[MAX_KBUFF];
/*
    if ((*off) > 0) {
        trace_printk(MODULE_NAME": Can not write with offset\n");
        return 0;
    }
*/
    if ((len > MAX_BUFF_ITEMS) || (len > MAX_KBUFF)) {
        trace_printk(MODULE_NAME": Too much items to write\n");
        return -ENOSPC;
    }
    if (copy_from_user(kbuffer, buf, len)) {
        trace_printk(MODULE_NAME": Could not copy from user\n");
        return -EINVAL;
    }
    (*off) += len;	

    if (down_interruptible(&mtx)) {
        trace_printk(MODULE_NAME": Interrupted in write mutex\n");
        return -EINTR;
    }

    while( nr_gaps_cbuffer_t(buffer) < len && cons_count>0 ) {
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

    insert_items_cbuffer_t(buffer, kbuffer, len);

    // Despertar a posibles consumidores bloqueados
    sem_signal(&sem_cons, &nr_cons_waiting);
    
    // liberar el MUTEX
    up(&mtx);

    return len;
}


/*****************************************************************************
 *
 * Module meta struct
 *
 ****************************************************************************/
static const struct file_operations proc_entry_fops = {
    .open = fifoproc_open,
    .release = fifoproc_release,
    .read = fifoproc_read,
    .write = fifoproc_write,
};



/*****************************************************************************
 *
 * Module init and cleanup
 *
 ****************************************************************************/
int init_fifoproc_module( void ) {

    /* init resources */
    buffer = create_cbuffer_t(MAX_BUFF_ITEMS);
    if (buffer == NULL) {
        printk(KERN_INFO MODULE_NAME": Can't create the list buffer");
        return -ENOMEM;
    }

    /* condvar-like semaphores to sync prods and cons */
    sema_init(&sem_cons, 0);
    sema_init(&sem_prod, 0);
   
    /* mutex-like semaphore for mutual exclusion */ 
    sema_init(&mtx, 1);


    /* create module entry */
    proc_entry = proc_create("fifoproc", 0666, NULL, &proc_entry_fops);
    if (proc_entry == NULL) {
        destroy_cbuffer_t(buffer);
        printk(KERN_INFO MODULE_NAME": Can't create /proc entry\n");
        return -ENOMEM;
    }

    trace_printk(MODULE_NAME": MODULE LOADED ==========\n");
    printk(KERN_INFO MODULE_NAME": Module loaded.\n");

    return 0;
}


void cleanup_fifoproc_module( void ) {

    /* remove module entry */
    remove_proc_entry(MODULE_NAME, NULL);

    /* free resources */
    destroy_cbuffer_t(buffer);

    trace_printk(MODULE_NAME": MODULE UNLOADED =========\n");
    printk(KERN_INFO MODULE_NAME": Module unloaded.\n");
}

module_init( init_fifoproc_module );
module_exit( cleanup_fifoproc_module );



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

