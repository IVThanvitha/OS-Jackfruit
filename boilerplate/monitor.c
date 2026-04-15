/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 */
#include <linux/workqueue.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/jiffies.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1: Define your linked-list node struct.
 * ============================================================== */
struct monitored_proc {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int soft_warned;
    char id[64];
    struct list_head list;
};

/* ==============================================================
 * TODO 2: Declare the global monitored list and a lock.
 * ============================================================== */
static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_list_lock);

/* --- internal device / timer state --- */
static struct delayed_work monitor_dwork;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Workqueue Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void monitor_work_fn(struct work_struct *work)
{
    struct monitored_proc *entry, *tmp;

    mutex_lock(&monitor_list_lock);
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        long rss_bytes = get_rss_bytes(entry->pid);

        if (rss_bytes == -1) {
            /* Process exited */
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (entry->hard_limit_bytes > 0 && rss_bytes > entry->hard_limit_bytes) {
            kill_process(entry->id, entry->pid, entry->hard_limit_bytes, rss_bytes);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (entry->soft_limit_bytes > 0 && rss_bytes > entry->soft_limit_bytes && !entry->soft_warned) {
            log_soft_limit_event(entry->id, entry->pid, entry->soft_limit_bytes, rss_bytes);
            entry->soft_warned = 1;
        }
    }
    mutex_unlock(&monitor_list_lock);

    schedule_delayed_work(&monitor_dwork, CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_req req;
    pid_t unreg_pid;
    struct monitored_proc *entry, *tmp;
    int found = 0;

    (void)f;

    if (cmd != MONITOR_IOCTL_REGISTER && cmd != MONITOR_IOCTL_UNREGISTER)
        return -EINVAL;

    if (cmd == MONITOR_IOCTL_REGISTER) {
        struct monitored_proc *new_proc;

        if (copy_from_user(&req, (struct monitor_req __user *)arg, sizeof(req)))
            return -EFAULT;

        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.id, req.pid, req.soft_limit_mib * 1024 * 1024, req.hard_limit_mib * 1024 * 1024);

        new_proc = kmalloc(sizeof(*new_proc), GFP_KERNEL);
        if (!new_proc)
            return -ENOMEM;

        new_proc->pid = req.pid;
        new_proc->soft_limit_bytes = req.soft_limit_mib * 1024 * 1024;
        new_proc->hard_limit_bytes = req.hard_limit_mib * 1024 * 1024;
        new_proc->soft_warned = 0;
        strncpy(new_proc->id, req.id, sizeof(new_proc->id) - 1);
        new_proc->id[sizeof(new_proc->id) - 1] = '\0';

        mutex_lock(&monitor_list_lock);
        list_for_each_entry(entry, &monitor_list, list) {
            if (entry->pid == new_proc->pid) {
                // Duplicate PID
                mutex_unlock(&monitor_list_lock);
                kfree(new_proc);
                return -EEXIST;
            }
        }
        list_add_tail(&new_proc->list, &monitor_list);
        mutex_unlock(&monitor_list_lock);

        return 0;
    }

    if (cmd == MONITOR_IOCTL_UNREGISTER) {
        if (copy_from_user(&unreg_pid, (pid_t __user *)arg, sizeof(pid_t)))
            return -EFAULT;

        printk(KERN_INFO
               "[container_monitor] Unregister request pid=%d\n", unreg_pid);

        mutex_lock(&monitor_list_lock);
        list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
            if (entry->pid == unreg_pid) {
                list_del(&entry->list);
                kfree(entry);
                found = 1;
                break;
            }
        }
        mutex_unlock(&monitor_list_lock);

        return found ? 0 : -ENOENT;
    }

    return -EINVAL;
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    INIT_DELAYED_WORK(&monitor_dwork, monitor_work_fn);
    schedule_delayed_work(&monitor_dwork, CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    struct monitored_proc *entry, *tmp;

    cancel_delayed_work_sync(&monitor_dwork);

    mutex_lock(&monitor_list_lock);
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitor_list_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
