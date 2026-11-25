/*
 * android-tether -- internet from an Android phone over USB, with no
 * kernel extension, no system extension and no entitlements.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * The system extension in driver/ is the proper way to do this, and it
 * cannot be loaded until Apple grants it three restricted entitlements.
 * This is the way that works today: open the phone's RNDIS interface from
 * user space (as rndis-probe does), lease an address from it over DHCP,
 * and bridge a utun interface to it.
 *
 *   utun  --IPv4-->  + Ethernet header  -->  RNDIS records -->  bulk OUT
 *                     (as many per transfer as the phone said it takes)
 *   bulk IN  -->  rndis_parse_transfer  -->  IPv4: strip header --> utun
 *                                         ARP: answer for our address
 *                                         DHCP: lease renewals
 *                                         anything else: counted, dropped
 *
 * Everything runs on one serial queue, as everything in the extension
 * runs on one dispatch queue, and for the same reason: no locks.
 *
 * Needs root, for utun and for the routes. Ctrl-C puts the Mac back the
 * way it was; so does pulling the cable; so, as far as the network
 * configuration goes, does the process dying.
 */

#import <Foundation/Foundation.h>
#import <IOKit/IOReturn.h>
#import <IOKit/usb/IOUSBHostFamilyDefinitions.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#import "rndis_usb.h"
#include "netconfig.h"
#include "netproto.h"
#include "utun.h"

#define RX_SLOTS             4      /* as ATD_RX_SLOTS in the extension */
#define TX_SLOTS             4      /* transfers in flight before utun waits */
#define TX_MAX_PACKETS       32     /* per transfer, whatever the phone offers */
#define TX_TIMEOUT_S         2.0
#define UTUN_BURST           128    /* packets per wake-up, so receive gets a turn */
#define RECORD_HLEN          (sizeof(struct rndis_packet_msg))
#define MAX_FRAMES_PER_XFER  64
#define RX_RETRY_DELAY_NS    (100 * NSEC_PER_MSEC)
#define DHCP_ATTEMPTS        5
#define DHCP_WAIT_S          1.0
#define ARP_ATTEMPTS         3
#define RENEW_RETRY_S        60
#define STATS_INTERVAL_S     5

/* ---------------------------------------------------------------- */
/* state, touched only on gQueue once the bridge is running          */
/* ---------------------------------------------------------------- */

static dispatch_queue_t gQueue;
static int gControl = -1;           /* the app's socket, if it started us */
static RNDISSession *gSession;
static int gUtun = -1;
static char gIfName[32];

static uint8_t gMac[6];             /* ours: OID_802_3_PERMANENT_ADDRESS */
static uint8_t gIp[4];
static uint8_t gRouter[4];
static uint8_t gGatewayMac[6];      /* where IPv4 frames are addressed */
static uint8_t gServerId[4];
static uint8_t gServerMac[6];
static uint8_t gDns[DHCP_MAX_DNS][4];
static uint32_t gDnsCount;
static uint32_t gLeaseSeconds;
static uint32_t gRenewXid;
static NSDate *gLeaseExpiry;

static NSMutableData *gRx[RX_SLOTS];
static BOOL gStopping;

/*
 * Transmit: records are written straight into one of TX_SLOTS buffers and
 * the buffer goes out as a single asynchronous transfer. gTxCur is the one
 * being filled, and is always -1 between handlers: whoever reserves room
 * flushes before returning to the queue.
 */
static NSMutableData *gTx[TX_SLOTS];
static int gTxFree[TX_SLOTS];
static int gTxFreeCount;
static int gTxCur = -1;
static size_t gTxLen;               /* bytes of records in gTxCur */
static uint32_t gTxCount;           /* records in gTxCur */
static uint64_t gTxFrameBytes;      /* Ethernet bytes in gTxCur, for the stats */
static uint32_t gTxMaxPackets = 1;
static size_t gTxMaxBytes;
static BOOL gAggregate = YES;
static dispatch_source_t gUtunSource;
static BOOL gUtunPaused;

static struct {
    uint64_t rxFrames, rxBytes, txFrames, txBytes;
    uint64_t rxDroppedIPv6, rxDroppedOther, rxBadTransfers, rxUtunFull;
    uint64_t txFailed, txTransfers;
} gStats, gLastStats;

static void shutdownBridge(const char *why, BOOL phoneGone, int status);
static void submitRead(int slot);
static void txFlush(void);

static void
printIp(const char *label, const uint8_t a[4])
{
    printf("%s%u.%u.%u.%u", label, a[0], a[1], a[2], a[3]);
}

/* ---------------------------------------------------------------- */
/* before the bridge: synchronous exchanges                          */
/* ---------------------------------------------------------------- */

/*
 * Send `frame`, then read for up to `wait` seconds and hand every frame
 * that arrives to `match` until it returns YES. Only used before the
 * asynchronous reads start, so the pipe is ours alone.
 */
static BOOL
exchange(const uint8_t *frame, size_t len, double wait,
         BOOL (^match)(const uint8_t *f, uint32_t flen))
{
    if (![gSession sendFrame:frame length:len])
        return NO;

    const uint32_t maxTransfer = gSession.maxTransfer;
    NSMutableData *buffer = [NSMutableData dataWithLength:maxTransfer];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:wait];

    while (deadline.timeIntervalSinceNow > 0) {
        NSUInteger got = 0;
        NSError *error = nil;
        if (![gSession.inPipe sendIORequestWithData:buffer
                                   bytesTransferred:&got
                                  completionTimeout:deadline.timeIntervalSinceNow
                                              error:&error]) {
            if (error.code == kIOReturnTimeout)
                return NO;
            USB_FAIL("bulk IN: %s", error.localizedDescription.UTF8String);
            return NO;
        }

        rndis_frame_t found[MAX_FRAMES_PER_XFER];
        uint32_t count = 0;
        rndis_parse_transfer(buffer.bytes, (uint32_t)got, found, MAX_FRAMES_PER_XFER,
                             maxTransfer, &count);
        for (uint32_t i = 0; i < count; i++) {
            if (match((const uint8_t *)buffer.bytes + found[i].offset, found[i].length))
                return YES;
        }
    }
    return NO;
}

static void
fillRequest(dhcp_request_t *req, uint8_t type, uint32_t xid)
{
    memset(req, 0, sizeof(*req));
    req->type = type;
    req->xid = xid;
    memcpy(req->client_mac, gMac, ETH_ALEN);
    memset(req->dst_mac, 0xFF, ETH_ALEN);
    memset(req->dst_ip, 0xFF, 4);
}

/* DISCOVER, OFFER, REQUEST, ACK. */
static BOOL
leaseAddress(void)
{
    uint8_t frame[RNDIS_ETHERNET_MAX_FRAME];
    __block dhcp_reply_t reply;

    for (int attempt = 1; attempt <= DHCP_ATTEMPTS; attempt++) {
        const uint32_t xid = arc4random();
        dhcp_request_t req;

        fillRequest(&req, DHCP_DISCOVER, xid);
        size_t len = dhcp_build(frame, sizeof(frame), &req);
        BOOL got = exchange(frame, len, DHCP_WAIT_S, ^BOOL(const uint8_t *f, uint32_t flen) {
            return dhcp_parse(f, flen, xid, &reply) == DHCP_PARSE_OK && reply.type == DHCP_OFFER;
        });
        if (!got) {
            USB_INFO("no DHCPOFFER (attempt %d of %d)", attempt, DHCP_ATTEMPTS);
            continue;
        }
        const dhcp_reply_t offer = reply;

        fillRequest(&req, DHCP_REQUEST, xid);
        memcpy(req.requested_ip, offer.your_ip, 4);
        memcpy(req.server_id, offer.server_id, 4);
        len = dhcp_build(frame, sizeof(frame), &req);
        got = exchange(frame, len, DHCP_WAIT_S, ^BOOL(const uint8_t *f, uint32_t flen) {
            return dhcp_parse(f, flen, xid, &reply) == DHCP_PARSE_OK &&
                   (reply.type == DHCP_ACK || reply.type == DHCP_NAK);
        });
        if (!got) {
            USB_INFO("no DHCPACK (attempt %d of %d)", attempt, DHCP_ATTEMPTS);
            continue;
        }
        if (reply.type == DHCP_NAK) {
            USB_INFO("DHCPNAK (attempt %d of %d)", attempt, DHCP_ATTEMPTS);
            continue;
        }

        memcpy(gIp, reply.your_ip, 4);
        memcpy(gServerId, reply.server_id, 4);
        memcpy(gServerMac, reply.src_mac, ETH_ALEN);
        /* No router option: on a tether, the server is the router. */
        memcpy(gRouter, reply.router[0] ? reply.router : reply.server_id, 4);
        memcpy(gDns, reply.dns, sizeof(gDns));
        gDnsCount = reply.dns_count;
        gLeaseSeconds = reply.lease_seconds ? reply.lease_seconds : 3600;
        gLeaseExpiry = [NSDate dateWithTimeIntervalSinceNow:gLeaseSeconds];

        printf("ok    DHCP lease ");
        printIp("", gIp);
        printIp(" via ", gRouter);
        for (uint32_t i = 0; i < reply.dns_count; i++)
            printIp(i == 0 ? ", DNS " : " ", reply.dns[i]);
        printf(", %u s\n", gLeaseSeconds);

        if (memcmp(reply.src_ip, gRouter, 4) == 0)
            memcpy(gGatewayMac, reply.src_mac, ETH_ALEN);
        return YES;
    }

    USB_FAIL("could not lease an address from the phone");
    return NO;
}

/* Only needed when the router is not the DHCP server we already heard. */
static BOOL
resolveGateway(void)
{
    static const uint8_t zero[6];
    if (memcmp(gGatewayMac, zero, 6) != 0)
        return YES;

    uint8_t frame[ETH_MIN_FRAME];
    const size_t len = arp_build_request(frame, sizeof(frame), gMac, gIp, gRouter);
    for (int attempt = 0; attempt < ARP_ATTEMPTS; attempt++) {
        if (exchange(frame, len, 1.0, ^BOOL(const uint8_t *f, uint32_t flen) {
                return arp_sender_mac(f, flen, gRouter, gGatewayMac);
            }))
            return YES;
    }
    USB_FAIL("no ARP reply from the router");
    return NO;
}

/* ---------------------------------------------------------------- */
/* the bridge: everything below runs on gQueue                       */
/* ---------------------------------------------------------------- */

/*
 * Room for one Ethernet frame of any size in the transfer being built, or
 * NULL if every buffer is on the wire. The caller writes the frame there
 * and calls txCommit with its length, or simply does not.
 */
static uint8_t *
txReserve(void)
{
    const size_t need = RECORD_HLEN + RNDIS_ETHERNET_MAX_FRAME + 1;     /* + the pad */

    if (gTxCur >= 0 && (gTxCount >= gTxMaxPackets || gTxLen + need > gTxMaxBytes))
        txFlush();
    if (gTxCur < 0) {
        if (gTxFreeCount == 0)
            return NULL;
        gTxCur = gTxFree[--gTxFreeCount];
        gTxLen = 0;
        gTxCount = 0;
        gTxFrameBytes = 0;
    }
    return (uint8_t *)gTx[gTxCur].mutableBytes + gTxLen + RECORD_HLEN;
}

static void
txCommit(size_t frameLen)
{
    uint8_t *record = (uint8_t *)gTx[gTxCur].mutableBytes + gTxLen;

    if (frameLen > RNDIS_ETHERNET_MAX_FRAME ||
        rndis_build_packet_header(record, RECORD_HLEN + frameLen,
                                  (uint32_t)frameLen) != RECORD_HLEN) {
        gStats.txFailed++;
        return;
    }
    gTxLen += RECORD_HLEN + frameLen;
    gTxCount++;
    gTxFrameBytes += frameLen;
}

static void
txDone(int slot, uint32_t frames, uint64_t frameBytes, IOReturn status, BOOL complete)
{
    if (gStopping)
        return;
    gTxFree[gTxFreeCount++] = slot;

    if (status == kIOReturnSuccess && complete) {
        gStats.txFrames += frames;
        gStats.txBytes += frameBytes;
        gStats.txTransfers++;
    } else {
        gStats.txFailed += frames;
        if (status == kIOReturnNoDevice || status == kIOReturnNotAttached) {
            shutdownBridge("the phone went away", YES, 0);
            return;
        }
        if (status == kUSBHostReturnPipeStalled)
            [gSession.outPipe clearStallWithError:nil];
    }

    /* A buffer is free again: let the Mac's packets in. */
    if (gUtunPaused) {
        gUtunPaused = NO;
        dispatch_resume(gUtunSource);
    }
}

/* Put the transfer being built on the wire. */
static void
txFlush(void)
{
    if (gTxCur < 0)
        return;

    const int slot = gTxCur;
    const uint32_t frames = gTxCount;
    const uint64_t frameBytes = gTxFrameBytes;
    size_t len = gTxLen;
    gTxCur = -1;

    if (frames == 0) {
        gTxFree[gTxFreeCount++] = slot;
        return;
    }

    /* A whole number of USB packets needs a short one after it: see
     * -[RNDISSession sendFrame:length:]. */
    uint8_t *bytes = gTx[slot].mutableBytes;
    if (len % gSession.outMaxPacketSize == 0)
        bytes[len++] = 0;

    /* The transfer's size is its NSData's length: a view, not the buffer. */
    NSMutableData *view = [NSMutableData dataWithBytesNoCopy:bytes length:len freeWhenDone:NO];
    NSError *error = nil;
    BOOL queued = [gSession.outPipe enqueueIORequestWithData:view
                                           completionTimeout:TX_TIMEOUT_S
                                                       error:&error
                                           completionHandler:^(IOReturn status, NSUInteger sent) {
        /* Naming the view here is also what keeps it alive until now. */
        const BOOL complete = sent == view.length;
        dispatch_async(gQueue, ^{ txDone(slot, frames, frameBytes, status, complete); });
    }];
    if (!queued)
        txDone(slot, frames, frameBytes, error != nil ? (IOReturn)error.code : kIOReturnError, NO);
}

/* One frame of our own making: an ARP reply, a DHCP renewal. */
static void
sendFrame(const uint8_t *frame, size_t len)
{
    uint8_t *room = txReserve();
    if (room == NULL || len > RNDIS_ETHERNET_MAX_FRAME) {
        gStats.txFailed++;
        txFlush();
        return;
    }
    memcpy(room, frame, len);
    txCommit(len);
    txFlush();
}

static void
handleDhcp(const dhcp_reply_t *reply)
{
    if (reply->type == DHCP_NAK) {
        shutdownBridge("the phone withdrew the lease (DHCPNAK)", NO, 1);
        return;
    }
    if (reply->type != DHCP_ACK)
        return;
    if (memcmp(reply->your_ip, gIp, 4) != 0) {
        /* Changing the address under live connections is a restart's job. */
        shutdownBridge("the phone renewed with a different address; run again", NO, 1);
        return;
    }
    gLeaseSeconds = reply->lease_seconds ? reply->lease_seconds : gLeaseSeconds;
    gLeaseExpiry = [NSDate dateWithTimeIntervalSinceNow:gLeaseSeconds];
    gRenewXid = 0;
}

static void
handleFrame(const uint8_t *f, uint32_t len)
{
    gStats.rxFrames++;
    gStats.rxBytes += len;

    switch (net_ethertype(f, len)) {
    case ETHERTYPE_IPV4: {
        /* Ours, or broadcast/multicast (the group bit). */
        if (memcmp(f, gMac, ETH_ALEN) != 0 && (f[0] & 1) == 0) {
            gStats.rxDroppedOther++;
            return;
        }
        if (gRenewXid != 0) {
            dhcp_reply_t reply;
            const dhcp_parse_result_t r = dhcp_parse(f, len, gRenewXid, &reply);
            if (r == DHCP_PARSE_OK) {
                handleDhcp(&reply);
                return;
            }
        }
        const uint32_t ipLen = net_ipv4_len(f, len);
        if (ipLen == 0) {
            gStats.rxDroppedOther++;
            return;
        }
        if (utun_write_ipv4(gUtun, f + ETH_HLEN, ipLen) != 0)
            gStats.rxUtunFull++;
        return;
    }

    case ETHERTYPE_ARP: {
        arp_sender_mac(f, len, gRouter, gGatewayMac);   /* keep it current */
        uint8_t reply[ETH_MIN_FRAME];
        const size_t replyLen = arp_build_reply(reply, sizeof(reply), f, len, gMac, gIp);
        if (replyLen > 0)
            sendFrame(reply, replyLen);
        return;
    }

    case ETHERTYPE_IPV6:
        gStats.rxDroppedIPv6++;
        return;

    default:
        gStats.rxDroppedOther++;
        return;
    }
}

static void
handleRead(int slot, IOReturn status, NSUInteger got)
{
    if (gStopping)
        return;

    if (status == kIOReturnAborted)
        return;
    if (status == kIOReturnNoDevice || status == kIOReturnNotAttached) {
        shutdownBridge("the phone went away", YES, 0);
        return;
    }
    if (status != kIOReturnSuccess) {
        gStats.rxBadTransfers++;
        if (status == kUSBHostReturnPipeStalled)
            [gSession.inPipe clearStallWithError:nil];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, RX_RETRY_DELAY_NS), gQueue, ^{
            submitRead(slot);
        });
        return;
    }

    rndis_frame_t found[MAX_FRAMES_PER_XFER];
    uint32_t count = 0;
    const uint8_t *bytes = gRx[slot].bytes;
    if (rndis_parse_transfer(bytes, (uint32_t)got, found, MAX_FRAMES_PER_XFER,
                             gSession.maxTransfer, &count) != RNDIS_PARSE_OK)
        gStats.rxBadTransfers++;
    for (uint32_t i = 0; i < count; i++)
        handleFrame(bytes + found[i].offset, found[i].length);

    submitRead(slot);
}

static void
submitRead(int slot)
{
    if (gStopping)
        return;

    NSError *error = nil;
    BOOL queued = [gSession.inPipe enqueueIORequestWithData:gRx[slot]
                                          completionTimeout:0
                                                      error:&error
                                          completionHandler:^(IOReturn status, NSUInteger got) {
        dispatch_async(gQueue, ^{ handleRead(slot, status, got); });
    }];
    if (!queued) {
        if (error.code == kIOReturnNoDevice || error.code == kIOReturnNotAttached) {
            shutdownBridge("the phone went away", YES, 0);
            return;
        }
        gStats.rxBadTransfers++;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, RX_RETRY_DELAY_NS), gQueue, ^{
            submitRead(slot);
        });
    }
}

/*
 * Everything the Mac routes to the phone. Packets are read straight into
 * the transfer being built, so whatever has queued up behind the last
 * transfer leaves in the next one: a lone packet goes at once, a burst of
 * them shares a transfer.
 */
static void
drainUtun(void)
{
    if (gStopping)
        return;

    for (int i = 0; i < UTUN_BURST; i++) {
        uint8_t *frame = txReserve();
        if (frame == NULL) {
            /* Every buffer is on the wire. Leave the rest in the kernel,
             * where TCP can see the queue, until txDone frees one. */
            gUtunPaused = YES;
            dispatch_suspend(gUtunSource);
            break;
        }
        const ssize_t n = utun_read_ipv4(gUtun, frame + ETH_HLEN,
                                         RNDIS_ETHERNET_MAX_FRAME - ETH_HLEN);
        if (n == UTUN_SKIPPED)
            continue;                   /* IPv6: the interface has no v6 address */
        if (n <= 0)
            break;
        net_put_eth(frame, gGatewayMac, gMac, ETHERTYPE_IPV4);
        txCommit(ETH_HLEN + (size_t)n);
    }
    txFlush();
}

static void
renewLease(void)
{
    if (gStopping)
        return;

    if (gLeaseExpiry.timeIntervalSinceNow <= 0) {
        shutdownBridge("the DHCP lease expired without being renewed", NO, 1);
        return;
    }

    /* RENEWING: unicast to the server that gave us the lease. */
    gRenewXid = arc4random() | 1;
    dhcp_request_t req;
    fillRequest(&req, DHCP_REQUEST, gRenewXid);
    memcpy(req.ciaddr, gIp, 4);
    memcpy(req.src_ip, gIp, 4);
    memcpy(req.dst_ip, gServerId, 4);
    memcpy(req.dst_mac, gServerMac, ETH_ALEN);

    uint8_t frame[RNDIS_ETHERNET_MAX_FRAME];
    const size_t len = dhcp_build(frame, sizeof(frame), &req);
    sendFrame(frame, len);

    /* Ask again in a minute unless the ACK (handleDhcp) resets the clock. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, RENEW_RETRY_S * NSEC_PER_SEC), gQueue, ^{
        if (gRenewXid != 0)
            renewLease();
    });
}

static void
scheduleRenewal(void)
{
    const uint64_t t1 = (uint64_t)gLeaseSeconds / 2;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(t1 * NSEC_PER_SEC)), gQueue, ^{
        if (gStopping)
            return;
        renewLease();
        scheduleRenewal();
    });
}

static void
printStats(void)
{
    /*
     * For the app, the raw counters every time, changed or not: it works
     * out the rates itself, and a rate has to be told when it is zero.
     * Lines that start with @ are for it and not for people.
     */
    if (gControl >= 0) {
        printf("@stat %llu %llu %llu %llu %llu\n", gStats.rxBytes, gStats.txBytes,
               gStats.rxFrames, gStats.txFrames,
               gStats.txFailed + gStats.rxBadTransfers + gStats.rxUtunFull);
        return;
    }
    if (memcmp(&gStats, &gLastStats, sizeof(gStats)) == 0)
        return;
    gLastStats = gStats;

    printf("      rx %llu frames %.1f MB   tx %llu frames %.1f MB",
           gStats.rxFrames, gStats.rxBytes / 1e6, gStats.txFrames, gStats.txBytes / 1e6);
    if (gStats.txTransfers && gStats.txFrames > gStats.txTransfers)
        printf(" (%.1f per transfer)", (double)gStats.txFrames / gStats.txTransfers);
    if (gStats.rxDroppedIPv6)
        printf("   ipv6 dropped %llu", gStats.rxDroppedIPv6);
    if (gStats.txFailed || gStats.rxBadTransfers || gStats.rxUtunFull)
        printf("   errors tx %llu rx %llu utun %llu",
               gStats.txFailed, gStats.rxBadTransfers, gStats.rxUtunFull);
    printf("\n");
    fflush(stdout);
}

/*
 * The one way out. Withdraw from configd first, so the Mac goes back to
 * Wi-Fi before anything else can fail; then tell the phone, if it is still
 * there; then let the interface go.
 */
static void
shutdownBridge(const char *why, BOOL phoneGone, int status)
{
    if (gStopping)
        return;
    gStopping = YES;

    printf("\n%s\n", why);
    netconfig_down();
    USB_OK("network configuration restored");

    if (!phoneGone && gSession != nil) {
        dhcp_request_t req;
        fillRequest(&req, DHCP_RELEASE, arc4random());
        memcpy(req.ciaddr, gIp, 4);
        memcpy(req.src_ip, gIp, 4);
        memcpy(req.dst_ip, gServerId, 4);
        memcpy(req.server_id, gServerId, 4);
        memcpy(req.dst_mac, gServerMac, ETH_ALEN);
        uint8_t frame[RNDIS_ETHERNET_MAX_FRAME];
        const size_t len = dhcp_build(frame, sizeof(frame), &req);
        if (len > 0 && [gSession sendFrame:frame length:len])
            USB_OK("DHCP lease released");
        [gSession setPacketFilter:0];
        [gSession halt];
    }
    [gSession close];

    if (gUtun >= 0)
        close(gUtun);
    gUtun = -1;

    printf("tx %llu frames, rx %llu frames\n", gStats.txFrames, gStats.rxFrames);
    exit(status);
}

/* ---------------------------------------------------------------- */

/*
 * --control: how the app drives this without a terminal.
 *
 * The app listens on a Unix socket in its own temporary directory and
 * passes the path; this end connects, and from then on the socket is both
 * where the output goes and the off switch. When the app closes its end
 * -- because Disconnect was clicked, or it quit, or it crashed -- that is
 * read here as end-of-file, and treated exactly as Ctrl-C is.
 *
 * Connecting rather than being handed a file to write is deliberate: this
 * process is root and the path is the user's, and root should not create
 * or truncate anything at a path somebody else chose.
 */
static BOOL
connectControl(const char *path)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path))
        return NO;
    strlcpy(addr.sun_path, path, sizeof(addr.sun_path));

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NO;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NO;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    gControl = fd;
    return YES;
}

static void
watchControl(void)
{
    if (gControl < 0)
        return;
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)gControl, 0, gQueue);
    dispatch_source_set_event_handler(source, ^{
        /* Anything at all, or nothing ever again: both mean stop. */
        shutdownBridge("stopping", NO, 0);
    });
    dispatch_resume(source);
    CFBridgingRetain(source);
}

static void
usage(void)
{
    fprintf(stderr,
            "usage: sudo android-tether [--seize] [--no-aggregate]\n"
            "\n"
            "  Plug in an Android phone, turn on USB tethering, then run this.\n"
            "  The Mac's traffic goes through the phone until you press Ctrl-C.\n"
            "  --seize         ask a kernel driver holding the interface to let go\n"
            "  --no-aggregate  one frame per USB transfer, whatever the phone offers\n"
            "  --control PATH  report to, and be stopped through, a Unix socket (the app)\n");
}

static void
watchSignal(int sig)
{
    signal(sig, SIG_IGN);
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0, gQueue);
    dispatch_source_set_event_handler(source, ^{
        shutdownBridge("stopping", NO, 0);
    });
    dispatch_resume(source);
    /* Lives for the life of the process. */
    CFBridgingRetain(source);
}

int
main(int argc, const char *argv[])
{
    @autoreleasepool {
        BOOL seize = NO;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--seize") == 0) {
                seize = YES;
            } else if (strcmp(argv[i], "--no-aggregate") == 0) {
                gAggregate = NO;
            } else if (strcmp(argv[i], "--control") == 0 && i + 1 < argc) {
                if (!connectControl(argv[++i])) {
                    fprintf(stderr, "android-tether: cannot reach the control socket\n");
                    return 1;
                }
            } else {
                usage();
                return 2;
            }
        }
        if (geteuid() != 0) {
            fprintf(stderr, "android-tether needs root, for the utun interface and the "
                            "routes: run it with sudo.\n");
            return 1;
        }
        /*
         * Started by the app, the effective user is root and the real one
         * may still be whoever clicked. Make them agree, so that nothing
         * run from here -- ifconfig, route -- has to wonder which to use.
         */
        if (getuid() != 0 && setuid(0) != 0) {
            perror("setuid");
            return 1;
        }

        setvbuf(stdout, NULL, _IOLBF, 0);
        signal(SIGPIPE, SIG_IGN);
        gQueue = dispatch_queue_create("android-tether", DISPATCH_QUEUE_SERIAL);

        /* ---- the phone ---- */

        gSession = [RNDISSession openWithSeize:seize onTerminate:^{
            dispatch_async(gQueue, ^{ shutdownBridge("the phone went away", YES, 0); });
        }];
        if (gSession == nil || ![gSession initialize] ||
            ![gSession setPacketFilter:RNDIS_FILTER_DEFAULT]) {
            [gSession close];
            return 1;
        }
        memcpy(gMac, gSession.mac, ETH_ALEN);

        /*
         * How many records the phone takes in one transfer is its to say
         * (MaxPacketsPerTransfer), and so is how big that transfer may be.
         * A device that wants records aligned gets one at a time: nothing
         * here pads between them.
         */
        const rndis_init_reply_t init = gSession.initReply;
        gTxMaxBytes = gSession.maxTransfer;
        if (gAggregate && init.packet_alignment_factor == 0 &&
            init.max_packets_per_transfer > 1)
            gTxMaxPackets = MIN(init.max_packets_per_transfer, TX_MAX_PACKETS);
        USB_OK("sending up to %u frame%s per transfer", gTxMaxPackets,
               gTxMaxPackets == 1 ? "" : "s");

        if (!leaseAddress() || !resolveGateway()) {
            [gSession halt];
            [gSession close];
            return 1;
        }

        /* ---- the Mac ---- */

        gUtun = utun_open(gIfName, sizeof(gIfName));
        if (gUtun < 0) {
            [gSession halt];
            [gSession close];
            return 1;
        }
        USB_OK("interface %s", gIfName);

        /* From here on, every way out goes through shutdownBridge. */
        watchSignal(SIGINT);
        watchSignal(SIGTERM);
        watchSignal(SIGHUP);
        watchControl();

        __block int configured = -1;
        dispatch_sync(gQueue, ^{
            configured = netconfig_up(gIfName, gIp, gRouter,
                                      (const uint8_t(*)[4])gDns, gDnsCount);
        });
        if (configured != 0) {
            dispatch_async(gQueue, ^{ shutdownBridge("could not configure the network", NO, 1); });
            dispatch_main();
        }
        USB_OK("routes and DNS point at the phone");

        /* ---- the bridge ---- */

        dispatch_async(gQueue, ^{
            for (int i = 0; i < RX_SLOTS; i++) {
                gRx[i] = [NSMutableData dataWithLength:gSession.maxTransfer];
                submitRead(i);
            }

            const size_t txSize = MAX((size_t)gSession.maxTransfer,
                                      RECORD_HLEN + RNDIS_ETHERNET_MAX_FRAME) + 1;
            for (int i = 0; i < TX_SLOTS; i++) {
                gTx[i] = [NSMutableData dataWithLength:txSize];
                gTxFree[gTxFreeCount++] = i;
            }

            gUtunSource =
                dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)gUtun, 0, gQueue);
            dispatch_source_set_event_handler(gUtunSource, ^{ drainUtun(); });
            dispatch_resume(gUtunSource);

            dispatch_source_t stats =
                dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gQueue);
            dispatch_source_set_timer(stats, dispatch_time(DISPATCH_TIME_NOW, 0),
                                      (gControl >= 0 ? 1 : STATS_INTERVAL_S) * NSEC_PER_SEC,
                                      NSEC_PER_SEC / 10);
            dispatch_source_set_event_handler(stats, ^{ printStats(); });
            dispatch_resume(stats);
            CFBridgingRetain(stats);

            scheduleRenewal();

            printf("\ntethering through the phone on %s. Ctrl-C to stop.\n\n", gIfName);
            if (gControl >= 0)
                printf("@up %s %u.%u.%u.%u %u.%u.%u.%u\n", gIfName,
                       gIp[0], gIp[1], gIp[2], gIp[3],
                       gRouter[0], gRouter[1], gRouter[2], gRouter[3]);
        });

        dispatch_main();
    }
}
