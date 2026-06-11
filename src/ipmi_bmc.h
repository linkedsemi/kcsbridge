/* SPDX-License-Identifier: Apache-2.0 */
/* IPMI BMC ioctl constants for Zephyr adaptation */

#define IPMI_BMC_IOCTL_SET_SMS_ATN    0x00
#define IPMI_BMC_IOCTL_CLEAR_SMS_ATN  0x01
#define IPMI_BMC_IOCTL_FORCE_ABORT    0x02

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
