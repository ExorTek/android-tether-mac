/*
 * rndis_usb.h -- an RNDIS session over IOUSBHost, from user space.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * Shared by rndis-probe and android-tether. It does what the extension
 * does at Start, in the same order -- driver/AndroidTetherDriver.cpp is
 * the reference, and a step that fails here points at the same step
 * there.
 */

#import <Foundation/Foundation.h>
#import <IOUSBHost/IOUSBHost.h>

#include "rndis_control.h"
#include "rndis_frame.h"

NS_ASSUME_NONNULL_BEGIN

/* One output style for both tools: a step, and whether it worked. */
extern int rndis_usb_failures;
#define USB_OK(fmt, ...)   printf("ok    " fmt "\n", ##__VA_ARGS__)
#define USB_FAIL(fmt, ...) do { printf("FAIL  " fmt "\n", ##__VA_ARGS__); rndis_usb_failures++; } while (0)
#define USB_INFO(fmt, ...) printf("      " fmt "\n", ##__VA_ARGS__)

@interface RNDISSession : NSObject

/*
 * Find an RNDIS control interface and its CDC Data interface, open both
 * and take the bulk pipes. nil, with the reason already printed, if any
 * of that fails. `onTerminate` runs, on an internal queue, when the phone
 * goes away.
 */
+ (nullable instancetype)openWithSeize:(BOOL)seize
                           onTerminate:(nullable void (^)(void))onTerminate;

/* Every USB interface the system can see, for when nothing matched. */
+ (void)listInterfaces;

/* REMOTE_NDIS_INITIALIZE, then OID_802_3_PERMANENT_ADDRESS. */
- (BOOL)initialize;

/* A QUERY with a 32-bit answer, printed; for information only. */
- (void)printQueryU32:(uint32_t)oid name:(const char *)name;

/* OID_GEN_CURRENT_PACKET_FILTER. */
- (BOOL)setPacketFilter:(uint32_t)filter;

/* Best effort, as in the extension's Stop. */
- (void)halt;

/*
 * Wrap one Ethernet frame in a data record and send it, synchronously.
 * Safe to call from any one thread at a time.
 */
- (BOOL)sendFrame:(const uint8_t *)frame length:(size_t)length;

- (void)close;

@property (readonly) IOUSBHostPipe *inPipe;
@property (readonly) IOUSBHostPipe *outPipe;
@property (readonly) uint32_t maxTransfer;      /* clamped as the extension does */
@property (readonly) uint32_t outMaxPacketSize; /* the bulk OUT endpoint's wMaxPacketSize */
@property (readonly) rndis_init_reply_t initReply;
@property (readonly) const uint8_t *mac;        /* 6 bytes, valid after -initialize */

@end

NS_ASSUME_NONNULL_END
