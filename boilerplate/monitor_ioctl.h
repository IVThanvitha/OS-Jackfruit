#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#define MONITOR_IOC_MAGIC 'm'

struct monitor_req {
    pid_t pid;
    unsigned long soft_limit_mib;
    unsigned long hard_limit_mib;
    char id[64];
};

#define MONITOR_IOCTL_REGISTER   _IOW(MONITOR_IOC_MAGIC, 1, struct monitor_req)
#define MONITOR_IOCTL_UNREGISTER _IOW(MONITOR_IOC_MAGIC, 2, pid_t)

#endif
