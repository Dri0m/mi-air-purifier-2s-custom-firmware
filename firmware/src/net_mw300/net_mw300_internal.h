/* net_mw300_internal.h - private glue between wifi_mw300.c, netio_mw300.c
 * and NetworkInterface.c.  Not a module interface. */
#ifndef NET_MW300_INTERNAL_H
#define NET_MW300_INTERNAL_H
#include <stdint.h>

#define NET_MAX_MCAST_GROUPS 4

/* NetworkInterface.c */
void net_mcast_init(void);                 /* create the lock; before IPInit */
int net_mcast_join(uint32_t group_be);     /* 0 ok, <0 table full */
void net_mcast_apply_hw(void);             /* (re)program Wi-Fi MAC filter */

/* wifi_mw300.c */
int net_wifi_driver_ready(void);           /* wlan_start() reported INITIALIZED */
#endif
