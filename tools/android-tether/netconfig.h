/*
 * netconfig.h -- making macOS send its traffic down the utun interface.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 */

#ifndef NETCONFIG_H
#define NETCONFIG_H

#include <stdint.h>

/*
 * Address the interface, route everything through it, and publish it to
 * configd as the primary service with the phone's DNS servers. Wi-Fi and
 * every other service are left exactly as they are; they simply stop
 * being primary while this one exists. Returns 0 on success.
 */
int netconfig_up(const char *ifname, const uint8_t ip[4], const uint8_t router[4],
                 const uint8_t (*dns)[4], uint32_t dns_count);

/*
 * Withdraw the service from configd. The routes and address go with the
 * interface when its socket is closed. Safe to call more than once, and
 * from a process that is exiting: the configd keys are also tied to this
 * process's session and vanish with it.
 */
void netconfig_down(void);

#endif /* NETCONFIG_H */
