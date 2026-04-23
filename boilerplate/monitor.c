/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Provided boilerplate:
 *   - device registration and teardown
 *   - timer setup
 *   - RSS helper
 *   - soft-limit and hard-limit event helpers
 *   - ioctl dispatch shell
 *
 * YOUR WORK: Fill in all sections marked // TODO.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/version.h>
#include <linux/timer.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * MONITORED ENTRY STRUCT (TODO 1 DONE)
 * ============================================================== */
struct monitor_entry {
    pid_t pid;
    char container_id[32];

    unsigned long soft_limit;
    unsigned long hard_limit;

    int soft_warned;

    struct list_head list;
};

/* ==============================================================
 * GLOBAL LIST + LOCK (TODO 2 DONE)
 * ============================================================== */
static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);

/* --- device state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ==============================================================
 * RSS HELPER (PROVIDED)
 * ============================================================== */
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

/* ==============================================================
 * LIMIT HELPERS
 * ============================================================== */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING "[monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

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

    printk(KERN_WARNING "[monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ==============================================================
 * TODO 3 DONE: TIMER CALLBACK
 * ============================================================== */
static void timer_callback(struct timer_list *t)
{
    struct monitor_entry *e, *tmp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(e, tmp, &monitor_list, list) {
        long rss = get_rss_bytes(e->pid);

        /* process exited */
        if (rss < 0) {
            list_del(&e->list);
            kfree(e);
            continue;
        }

        /* soft limit */
        if (rss > e->soft_limit && !e->soft_warned) {
            log_soft_limit_event(e->container_id, e->pid, e->soft_limit, rss);
            e->soft_warned = 1;
        }

        /* hard limit */
        if (rss > e->hard_limit) {
            kill_process(e->container_id, e->pid, e->hard_limit, rss);
            list_del(&e->list);
            kfree(e);
        }
    }

    mutex_unlock(&monitor_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ==============================================================
 * TODO 4 + 5 DONE: IOCTL
 * ============================================================== */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    /* REGISTER */
    if (cmd == MONITOR_REGISTER) {
        struct monitor_entry *e;

        e = kmalloc(sizeof(*e), GFP_KERNEL);
        if (!e)
            return -ENOMEM;

        e->pid = req.pid;
        strncpy(e->container_id, req.container_id, 31);

        e->soft_limit = req.soft_limit_bytes;
        e->hard_limit = req.hard_limit_bytes;

        e->soft_warned = 0;

        mutex_lock(&monitor_lock);
        list_add(&e->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        return 0;
    }

    /* UNREGISTER */
    if (cmd == MONITOR_UNREGISTER) {
        struct monitor_entry *e, *tmp;
        int found = 0;

        mutex_lock(&monitor_lock);

        list_for_each_entry_safe(e, tmp, &monitor_list, list) {
            if (e->pid == req.pid &&
                strncmp(e->container_id, req.container_id, 32) == 0) {

                list_del(&e->list);
                kfree(e);
                found = 1;
                break;
            }
        }

        mutex_unlock(&monitor_lock);
        return found ? 0 : -ENOENT;
    }

    return -EINVAL;
}

/* ==============================================================
 * FILE OPS
 * ============================================================== */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ==============================================================
 * INIT
 * ============================================================== */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    if (IS_ERR(cl))
        return PTR_ERR(cl);

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME)))
        return -1;

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0)
        return -1;

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[monitor] loaded\n");
    return 0;
}

/* ==============================================================
 * TODO 6 DONE: EXIT CLEANUP
 * ============================================================== */
static void __exit monitor_exit(void)
{
    struct monitor_entry *e, *tmp;

    timer_delete_sync(&monitor_timer);

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(e, tmp, &monitor_list, list) {
        list_del(&e->list);
        kfree(e);
    }

    mutex_unlock(&monitor_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[monitor] unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Multi-container memory monitor (complete)");
