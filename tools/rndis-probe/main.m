/*
 * rndis-probe -- talk RNDIS to a phone from user space.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * The extension cannot be loaded without three entitlements Apple has to
 * grant, or a Mac with SIP and AMFI turned off. Most of what could be
 * wrong with it does not need either to find out: this runs the
 * extension's own handshake (see ../common/rndis_usb.m) against a real
 * phone, sends a DHCPDISCOVER, and checks that an OFFER comes back.
 *
 * What this proves: the protocol layer, both directions. What it cannot:
 * anything NetworkingDriverKit decides -- queue order, the link header
 * length, whether the interface registers at all.
 */

#import <Foundation/Foundation.h>
#import <IOKit/IOReturn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import "rndis_usb.h"
#include "netproto.h"

#define BULK_IN_SLICE_S      0.5
#define MAX_FRAMES_PER_XFER  64

static const char *
etherTypeName(uint16_t t)
{
    switch (t) {
    case ETHERTYPE_IPV4: return "IPv4";
    case ETHERTYPE_ARP:  return "ARP";
    case ETHERTYPE_IPV6: return "IPv6";
    default:             return "?";
    }
}

static void
usage(void)
{
    fprintf(stderr,
            "usage: rndis-probe [-t seconds] [--seize] [--list]\n"
            "\n"
            "  Plug in an Android phone, turn on USB tethering, then run this.\n"
            "  -t SECONDS   how long to listen for frames (default 10)\n"
            "  --seize      ask a kernel driver holding the interface to let go\n"
            "  --list       print every USB interface and exit\n");
}

int
main(int argc, const char *argv[])
{
    @autoreleasepool {
        double listenSeconds = 10;
        BOOL seize = NO;

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
                listenSeconds = atof(argv[++i]);
            } else if (strcmp(argv[i], "--seize") == 0) {
                seize = YES;
            } else if (strcmp(argv[i], "--list") == 0) {
                [RNDISSession listInterfaces];
                return 0;
            } else {
                usage();
                return 2;
            }
        }

        RNDISSession *session = [RNDISSession openWithSeize:seize onTerminate:nil];
        if (session == nil || ![session initialize])
            return 1;

        /* Informational: what a later version might use. */
        [session printQueryU32:OID_GEN_MAXIMUM_FRAME_SIZE name:"OID_GEN_MAXIMUM_FRAME_SIZE"];
        [session printQueryU32:OID_GEN_PHYSICAL_MEDIUM name:"OID_GEN_PHYSICAL_MEDIUM"];

        if (![session setPacketFilter:RNDIS_FILTER_DEFAULT])
            return 1;

        /* ---- TX: one DHCPDISCOVER ---- */

        dhcp_request_t req;
        memset(&req, 0, sizeof(req));
        req.type = DHCP_DISCOVER;
        req.xid = arc4random();
        memcpy(req.client_mac, session.mac, ETH_ALEN);
        memset(req.dst_mac, 0xFF, ETH_ALEN);
        memset(req.dst_ip, 0xFF, 4);

        uint8_t frame[RNDIS_ETHERNET_MAX_FRAME];
        const size_t frameLen = dhcp_build(frame, sizeof(frame), &req);
        if (frameLen == 0 || ![session sendFrame:frame length:frameLen])
            USB_FAIL("sending DHCPDISCOVER");
        else
            USB_OK("sent DHCPDISCOVER, xid 0x%08x, %zu bytes on the wire", req.xid,
                   frameLen + sizeof(struct rndis_packet_msg));

        /* ---- RX: listen, parse every transfer with rndis_parse_transfer ---- */

        const uint32_t maxTransfer = session.maxTransfer;
        NSMutableData *rxData = [NSMutableData dataWithLength:maxTransfer];
        const uint8_t *rx = rxData.bytes;

        USB_INFO("listening for %.0f s", listenSeconds);
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:listenSeconds];
        unsigned transfers = 0, frames = 0, bad = 0;
        BOOL offered = NO;

        while ([deadline timeIntervalSinceNow] > 0) {
            NSUInteger got = 0;
            NSError *error = nil;
            if (![session.inPipe sendIORequestWithData:rxData
                                      bytesTransferred:&got
                                     completionTimeout:BULK_IN_SLICE_S
                                                 error:&error]) {
                if (error.code == kIOReturnTimeout)
                    continue;
                USB_FAIL("bulk IN: %s (0x%08lx)", error.localizedDescription.UTF8String,
                         (long)error.code);
                break;
            }
            if (got == 0)
                continue;
            transfers++;

            rndis_frame_t found[MAX_FRAMES_PER_XFER];
            uint32_t count = 0;
            const rndis_parse_result_t parsed =
                rndis_parse_transfer(rx, (uint32_t)got, found, MAX_FRAMES_PER_XFER,
                                     maxTransfer, &count);
            if (parsed != RNDIS_PARSE_OK) {
                bad++;
                USB_INFO("transfer of %lu bytes: %s after %u frames",
                         (unsigned long)got, rndis_parse_result_name(parsed), count);
            }

            for (uint32_t i = 0; i < count; i++) {
                const uint8_t *f = rx + found[i].offset;
                const uint32_t len = found[i].length;
                frames++;
                if (len < ETH_HLEN) {
                    USB_INFO("frame %u: %u bytes, shorter than an Ethernet header", frames, len);
                    continue;
                }
                const uint16_t type = net_ethertype(f, len);
                USB_INFO("frame %u: %4u bytes  %02x:%02x:%02x:%02x:%02x:%02x -> "
                         "%02x:%02x:%02x:%02x:%02x:%02x  %04x %s",
                         frames, len, f[6], f[7], f[8], f[9], f[10], f[11],
                         f[0], f[1], f[2], f[3], f[4], f[5], type, etherTypeName(type));

                dhcp_reply_t reply;
                if (!offered && dhcp_parse(f, len, req.xid, &reply) == DHCP_PARSE_OK &&
                    reply.type == DHCP_OFFER) {
                    offered = YES;
                    USB_OK("DHCPOFFER %u.%u.%u.%u from %u.%u.%u.%u",
                           reply.your_ip[0], reply.your_ip[1], reply.your_ip[2], reply.your_ip[3],
                           reply.server_id[0], reply.server_id[1], reply.server_id[2],
                           reply.server_id[3]);
                }
            }
        }

        if (frames > 0)
            USB_OK("received %u frames in %u transfers", frames, transfers);
        else
            USB_FAIL("no frames received in %.0f s", listenSeconds);
        if (bad > 0)
            USB_FAIL("%u transfers did not parse cleanly", bad);
        if (!offered)
            USB_FAIL("no DHCPOFFER for our DISCOVER");

        [session halt];
        [session close];

        printf("\n%s: %d failure%s\n", rndis_usb_failures ? "FAILED" : "PASSED",
               rndis_usb_failures, rndis_usb_failures == 1 ? "" : "s");
        return rndis_usb_failures ? 1 : 0;
    }
}
