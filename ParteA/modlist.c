/*=====================================================================================
    MODULE: modlist

    DESCRIPTION:
        Maintains a kernel linked list of integers

    USAGE:
        echo add <number> > /proc/modlist       inserts <number> at the end
        echo remove <number> > /proc/modlist    remove all ocurrences of <number>
        cat /proc/modlist                       prints the whole list
        echo cleanup > /proc/modlist            delete the list content

    CONDITIONAL COMPILATION
        STRING_MODE
            If defined, the list contains string values, if not, integer values.
        TEST_NO_LOCK
            If defined, the spin locks are not used, to test the failures it causes.

    COMMENTARIES
        Compiling with TEST_NO_LOCK does not show the expected failures either...
=======================================================================================
*/


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/string.h>
#include <asm-generic/uaccess.h>
#include <linux/ftrace.h>
#include <linux/spinlock.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Modlist kernel module - FDI-UCM");
MODULE_AUTHOR("Daniel Pinto, Javier Bermudez");


#ifndef TEST_NO_LOCK
DEFINE_RWLOCK(sp);
#endif


#define BUFFER_LENGHT   50
#define READ_BUFFER_LENGHT 200

#ifdef STRING_MODE
 #define STRING_LENGHT 50
#endif

#ifdef STRING_MODE
 #define DATA_PRINT_FORMAT "%s"
#else
 #define DATA_PRINT_FORMAT "%d"
#endif


static struct proc_dir_entry *proc_entry;

/* Linked list */
struct list_head mylist;

/* List nodes */
typedef struct {
#ifdef STRING_MODE
    char data[STRING_LENGHT];
#else
    int data;
#endif
    struct list_head links;
}list_item_t;


int botupcmp(void *priv, struct list_head *a, struct list_head *b) {
    list_item_t *entry_a, *entry_b;

    entry_a = list_entry(a, list_item_t, links);
    entry_b = list_entry(b, list_item_t, links);

#ifndef STRING_MODE
    return (entry_a->data - entry_b->data);
#else
    return strcasecmp(entry_a->data, entry_b->data);
#endif
}


static ssize_t modlist_read(struct file *filp, char __user *buf, size_t len, loff_t *off) {

    list_item_t *pos;
    int nchars, nbytes, ret = 0;
    char *buff_pos = buf;

#ifndef STRING_MODE
    char int_buf[21]; //max digits posible in 64bit integer
#else
    char int_buf[STRING_LENGHT+1];
#endif

    // we return the string in just one call
    if ((*off) > 0)
        return 0;


    // lock for read
#ifndef TEST_NO_LOCK
    read_lock(&sp);
#endif

    list_for_each_entry(pos, &mylist, links) {

        // Check if there is enough space in buffer
#ifdef STRING_MODE
        nbytes = strlen(pos->data) + sizeof(char);  // string lenght + '\n'
        nchars = nbytes/sizeof(char);

        strcpy(int_buf, pos->data);
        int_buf[nchars-1] = '\n';

#else
        nchars = sprintf(int_buf, "%d\n", pos->data);
        nbytes = nchars*sizeof(char);   // string lenght + '\n'
#endif
        if ( (nbytes) > (len-ret) )
            break;

        // Copy to buff    
        if( copy_to_user(buff_pos, int_buf, nbytes) ) {
            return -EFAULT;
        }

        buff_pos += nchars;
        ret += nbytes;
    }
    // unlock
#ifndef TEST_NO_LOCK
    read_unlock(&sp);
#endif
    
    //int_buf[0] = '\0';
    //if( copy_to_user(buff_pos, int_buf, sizeof(char)) ) {
    //    return -EFAULT;
    //}

    *off+=ret;

    return ret;
}


static ssize_t modlist_write(struct file *filp, const char __user *buf, size_t len, loff_t *off) {

    char aux_buffer[BUFFER_LENGHT];
    char command[BUFFER_LENGHT];

    list_item_t *pos, *temp;

#ifndef STRING_MODE
    int value;
#else
    char value[STRING_LENGHT];
#endif

    if (len >= BUFFER_LENGHT) {
        printk(KERN_INFO "Modlist: input too large\n");
        return -ENOSPC;
    }

    if (copy_from_user(aux_buffer, buf, len)) {
        return -EFAULT;
    }
	aux_buffer[len] = '\0';
    *off+=len;

    // parse argument (it seems to work fine if there is no value in input)
#ifndef STRING_MODE
    sscanf(aux_buffer, "%s %d", command, &value);
#else
    sscanf(aux_buffer, "%s %s", command, value);
#endif

    // COMMAND: Add <number>
    if (!strcasecmp(command, "add")) {
        trace_printk("Modlist: add "DATA_PRINT_FORMAT"\n", value);

        temp = vmalloc( sizeof(list_item_t) );
        if (temp == NULL) {
            printk(KERN_INFO "Modlist: Can't add item to list\n");
            return -ENOMEM;
        }
#ifndef STRING_MODE
        temp->data = value;
#else
        memcpy(temp->data, value, STRING_LENGHT*sizeof(char));
#endif

#ifndef TEST_NO_LOCK    
        write_lock(&sp);
#endif
        list_add_tail(&(temp->links), &mylist);
#ifndef TEST_NO_LOCK
		write_unlock(&sp);
#endif
    }
    // COMMAND: remove <number>
    else if (!strcasecmp(command, "remove")) {
#ifndef TEST_NO_LOCK
        write_lock(&sp);
#endif
        list_for_each_entry_safe(pos, temp, &mylist, links) {
#ifndef STRING_MODE
            if (pos->data == value) {
#else
            if (!strcasecmp(pos->data, value) ) {
#endif  
                trace_printk("Modlist: removed "DATA_PRINT_FORMAT"\n", value);
                list_del(&(pos->links));

                vfree(pos);
            }
        }
#ifndef TEST_NO_LOCK
        write_unlock(&sp);
#endif
    }
    // COMMAND: cleanup
    else if (!strcasecmp(command, "cleanup")) {
        trace_printk("Modlist: cleanup\n");
#ifndef TEST_NO_LOCK
        write_lock(&sp);
#endif
        list_for_each_entry_safe(pos, temp, &mylist, links) {
            trace_printk("Modlist: removed "DATA_PRINT_FORMAT"\n", pos->data);
            list_del(&(pos->links));
            vfree(pos);
        }
#ifndef TEST_NO_LOCK
        write_unlock(&sp);
#endif
    }
    // COMMAND: sort
    else if (!strcasecmp(command, "sort")) {
        trace_printk("Modlist: sort\n");
#ifndef TEST_NO_LOCK
        write_lock(&sp);
#endif
        list_sort(NULL, &mylist, botupcmp);
#ifndef TEST_NO_LOCK
		write_unlock(&sp);
#endif
    }

    return len;
}


static const struct file_operations proc_entry_fops = {
    .read = modlist_read,
    .write = modlist_write,
};

int init_modlist_module( void ){

    // init resources
    INIT_LIST_HEAD(&mylist);

    proc_entry = proc_create("modlist", 0666, NULL, &proc_entry_fops);
    if (proc_entry == NULL) {
        printk(KERN_INFO "Modlist: Can't create /proc entry\n");
        return -ENOMEM;
    }

#ifdef STRING_MODE
    trace_printk("Modlist: MODULE LOADED (string) =========\n");
#else
    trace_printk("Modlist: MODULE LOADED (int) ==========\n");
#endif
    printk(KERN_INFO "Modlist: Module loaded.\n");
    return 0;
}

void exit_modlist_module( void ){
    
    list_item_t *temp, *pos;

    remove_proc_entry("modlist", NULL);

    // free list resources
    list_for_each_entry_safe(pos, temp, &mylist, links) {
        list_del(&(pos->links));
        vfree(pos);
    }

#ifdef STRING_MODE
    trace_printk("Modlist: MODULE UNLOADED (string) =========\n");
#else
    trace_printk("Modlist: MODULE UNLOADED (int) =========\n");
#endif
    printk(KERN_INFO "Modlist: Module unloaded.\n");
}

module_init( init_modlist_module );
module_exit( exit_modlist_module );


