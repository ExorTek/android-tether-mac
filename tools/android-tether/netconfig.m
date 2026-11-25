/*
 * netconfig.m
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * Two layers, deliberately:
 *
 *  * Routes 0.0.0.0/1 and 128.0.0.0/1 through the interface. Together they
 *    cover every address and each is more specific than any default route,
 *    so traffic goes to the phone whatever configd decides. They belong to
 *    the interface and disappear with it.
 *
 *  * A service in configd's dynamic store with OverridePrimary set -- the
 *    mechanism VPN clients use. It is what makes macOS use the phone's DNS
 *    servers and treat this as the primary connection. Its keys are
 *    session keys, so configd drops them if this process dies without
 *    cleaning up.
 */

#import <Foundation/Foundation.h>
#import <SystemConfiguration/SystemConfiguration.h>

#include "netconfig.h"

#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static NSString *const kServiceID = @"com.exortek.androidtether";
static SCDynamicStoreRef gStore;

static NSString *
ipString(const uint8_t a[4])
{
    return [NSString stringWithFormat:@"%u.%u.%u.%u", a[0], a[1], a[2], a[3]];
}

static NSString *
stateKey(NSString *entity)
{
    return [NSString stringWithFormat:@"State:/Network/Service/%@/%@", kServiceID, entity];
}

/* Run a command, quietly unless it fails. */
static int
run(NSArray<NSString *> *argv)
{
    const char *args[16];
    NSUInteger n = 0;
    for (NSString *a in argv) {
        if (n + 1 >= sizeof(args) / sizeof(args[0]))
            return -1;
        args[n++] = a.UTF8String;
    }
    args[n] = NULL;

    /* route(8) prints "add net ..." on success even with -q. */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid;
    int status = 0;
    const int spawned = posix_spawn(&pid, args[0], &actions, NULL, (char *const *)args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0 ||
        waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "failed: %s\n", [argv componentsJoinedByString:@" "].UTF8String);
        return -1;
    }
    return 0;
}

int
netconfig_up(const char *ifname, const uint8_t ip[4], const uint8_t router[4],
             const uint8_t (*dns)[4], uint32_t dns_count)
{
    NSString *iface = @(ifname);
    NSString *addr = ipString(ip);
    NSString *gw = ipString(router);

    /* The phone forwards at most a 1500-byte payload (OID_GEN_MAXIMUM_FRAME_SIZE). */
    if (run(@[ @"/sbin/ifconfig", iface, @"inet", addr, gw,
               @"netmask", @"255.255.255.255", @"mtu", @"1500", @"up" ]) != 0)
        return -1;

    if (run(@[ @"/sbin/route", @"-q", @"-n", @"add", @"-net", @"0.0.0.0/1",
               @"-interface", iface ]) != 0 ||
        run(@[ @"/sbin/route", @"-q", @"-n", @"add", @"-net", @"128.0.0.0/1",
               @"-interface", iface ]) != 0)
        return -1;

    NSMutableArray<NSString *> *servers = [NSMutableArray array];
    for (uint32_t i = 0; i < dns_count; i++)
        [servers addObject:ipString(dns[i])];
    if (servers.count == 0)
        [servers addObject:gw];     /* Android's gateway also answers DNS */

    NSDictionary *options = @{ (__bridge NSString *)kSCDynamicStoreUseSessionKeys : @YES };
    gStore = SCDynamicStoreCreateWithOptions(NULL, CFSTR("android-tether"),
                                             (__bridge CFDictionaryRef)options, NULL, NULL);
    if (gStore == NULL) {
        fprintf(stderr, "SCDynamicStoreCreate: %s\n", SCErrorString(SCError()));
        return -1;
    }

    NSDictionary *ipv4 = @{
        (__bridge NSString *)kSCPropInterfaceName : iface,
        (__bridge NSString *)kSCPropNetIPv4Addresses : @[ addr ],
        (__bridge NSString *)kSCPropNetIPv4DestAddresses : @[ gw ],
        (__bridge NSString *)kSCPropNetIPv4SubnetMasks : @[ @"255.255.255.255" ],
        (__bridge NSString *)kSCPropNetIPv4Router : gw,
        (__bridge NSString *)kSCPropNetOverridePrimary : @1,
    };
    NSDictionary *dnsDict = @{
        (__bridge NSString *)kSCPropNetDNSServerAddresses : servers,
    };
    NSDictionary *values = @{ stateKey(@"IPv4") : ipv4, stateKey(@"DNS") : dnsDict };

    if (!SCDynamicStoreSetMultiple(gStore, (__bridge CFDictionaryRef)values, NULL, NULL)) {
        fprintf(stderr, "publishing to configd: %s\n", SCErrorString(SCError()));
        return -1;
    }
    return 0;
}

void
netconfig_down(void)
{
    if (gStore == NULL)
        return;
    NSArray *keys = @[ stateKey(@"IPv4"), stateKey(@"DNS") ];
    SCDynamicStoreSetMultiple(gStore, NULL, (__bridge CFArrayRef)keys, NULL);
    CFRelease(gStore);
    gStore = NULL;
}
