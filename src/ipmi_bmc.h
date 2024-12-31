// from: https://github.com/torvalds/linux/blob/master/include/uapi/linux/ipmi_bmc.h
 #define IPMI_BMC_IOCTL_SET_SMS_ATN    0x00
 #define IPMI_BMC_IOCTL_CLEAR_SMS_ATN  0x01
 #define IPMI_BMC_IOCTL_FORCE_ABORT    0x02

// from: https://github.com/systemd/systemd/blob/main/src/systemd/sd-event.h
 enum {
        /* And everything in-between and outside is good too */
        SD_EVENT_PRIORITY_IMPORTANT = -100,
        SD_EVENT_PRIORITY_NORMAL = 0,
        SD_EVENT_PRIORITY_IDLE = 100
};

// from：https://github.com/fd00/cygepoll/blob/master/sys/epoll.h
#define EPOLLIN  0x01
#define EPOLLOUT 0x02
#define EPOLLERR 0x04
#define EPOLLET  0x08