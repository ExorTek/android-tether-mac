/*
 * rndis_usb.m
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * macOS attaches no driver of its own to an Android RNDIS interface, so an
 * ordinary process can open it. Nothing here needs an entitlement; the
 * one thing that would stop it is a kernel driver already holding the
 * interface, and that is reported by name rather than as a bare error.
 */

#import "rndis_usb.h"

#import <IOKit/IOKitLib.h>
#import <IOKit/IOMessage.h>
#import <IOKit/usb/IOUSBHostFamilyDefinitions.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int rndis_usb_failures = 0;

/* The same figures the extension uses. */
#define USB_CLASS_CDC_DATA     0x0A
#define CONTROL_POLL_MS        5
#define CONTROL_POLL_ATTEMPTS  200
#define CONTROL_TIMEOUT_S      5.0
#define BULK_OUT_TIMEOUT_S     2.0

/* The interface triples from driver/Info.plist. Keep the two in step. */
static const struct { uint8_t cls, sub, proto; const char *name; } kTriples[] = {
    { 0xE0, 0x01, 0x03, "RNDIS over CDC ACM" },
    { 0xEF, 0x04, 0x01, "Microsoft RNDIS" },
    { 0x02, 0x02, 0xFF, "CDC ACM, vendor protocol" },
};

/* ---------------------------------------------------------------- */
/* finding the phone                                                 */
/* ---------------------------------------------------------------- */

static int
intProperty(io_service_t service, const char *key)
{
    CFStringRef cfKey = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, cfKey, NULL, 0);
    CFRelease(cfKey);

    int result = -1;
    if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(value, kCFNumberIntType, &result);
    if (value != NULL)
        CFRelease(value);
    return result;
}

static uint64_t
parentID(io_service_t service)
{
    io_registry_entry_t parent = IO_OBJECT_NULL;
    uint64_t id = 0;

    if (IORegistryEntryGetParentEntry(service, kIOServicePlane, &parent) == KERN_SUCCESS) {
        IORegistryEntryGetRegistryEntryID(parent, &id);
        IOObjectRelease(parent);
    }
    return id;
}

static void
describeOwner(io_service_t service, const char *what)
{
    io_iterator_t children = IO_OBJECT_NULL;
    if (IORegistryEntryGetChildIterator(service, kIOServicePlane, &children) != KERN_SUCCESS)
        return;

    io_object_t child;
    while ((child = IOIteratorNext(children)) != IO_OBJECT_NULL) {
        io_name_t className;
        if (IOObjectGetClass(child, className) == KERN_SUCCESS)
            USB_INFO("%s is held by %s", what, className);
        IOObjectRelease(child);
    }
    IOObjectRelease(children);
}

static NSArray<NSNumber *> *
allInterfaces(void)
{
    NSMutableArray<NSNumber *> *all = [NSMutableArray array];
    io_iterator_t iterator = IO_OBJECT_NULL;

    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching(kIOUSBHostInterfaceClassName),
                                     &iterator) != KERN_SUCCESS)
        return all;

    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL)
        [all addObject:@(service)];
    IOObjectRelease(iterator);
    return all;
}

/* Both come back retained. */
static BOOL
findInterfaces(io_service_t *control, io_service_t *data)
{
    *control = IO_OBJECT_NULL;
    *data = IO_OBJECT_NULL;

    NSArray<NSNumber *> *all = allInterfaces();
    uint64_t device = 0;

    for (NSNumber *n in all) {
        const io_service_t s = n.unsignedIntValue;
        const int cls = intProperty(s, "bInterfaceClass");
        const int sub = intProperty(s, "bInterfaceSubClass");
        const int proto = intProperty(s, "bInterfaceProtocol");

        for (size_t i = 0; i < sizeof(kTriples) / sizeof(kTriples[0]); i++) {
            if (cls == kTriples[i].cls && sub == kTriples[i].sub &&
                proto == kTriples[i].proto && *control == IO_OBJECT_NULL) {
                *control = s;
                IOObjectRetain(s);
                device = parentID(s);
                USB_OK("control interface %d: %02X/%02X/%02X (%s)",
                       intProperty(s, "bInterfaceNumber"), cls, sub, proto, kTriples[i].name);
            }
        }
    }

    /* The extension takes the first CDC Data interface on the device. */
    if (*control != IO_OBJECT_NULL) {
        for (NSNumber *n in all) {
            const io_service_t s = n.unsignedIntValue;
            if (*data == IO_OBJECT_NULL && parentID(s) == device &&
                intProperty(s, "bInterfaceClass") == USB_CLASS_CDC_DATA) {
                *data = s;
                IOObjectRetain(s);
                USB_OK("data interface %d: CDC Data", intProperty(s, "bInterfaceNumber"));
            }
        }
    }

    for (NSNumber *n in all)
        IOObjectRelease(n.unsignedIntValue);

    return *control != IO_OBJECT_NULL && *data != IO_OBJECT_NULL;
}

static IOUSBHostInterface *
openInterface(io_service_t service, const char *what, BOOL seize,
              IOUSBHostInterestHandler handler)
{
    NSError *error = nil;
    IOUSBHostInterface *iface = [[IOUSBHostInterface alloc]
        initWithIOService:service
                  options:seize ? IOUSBHostObjectInitOptionsDeviceSeize
                                : IOUSBHostObjectInitOptionsNone
                    queue:nil
                    error:&error
          interestHandler:handler];
    if (iface == nil) {
        USB_FAIL("opening the %s interface: %s (0x%08lx)", what,
                 error.localizedDescription.UTF8String, (long)error.code);
        describeOwner(service, what);
        return nil;
    }
    USB_OK("opened the %s interface", what);
    return iface;
}

static void
hexdump(const uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0; i < len && i < 64; i += 16) {
        char line[80] = "";
        for (uint32_t j = i; j < i + 16 && j < len; j++)
            snprintf(line + strlen(line), sizeof(line) - strlen(line), "%02x ", p[j]);
        USB_INFO("  %04x  %s", i, line);
    }
}

/* ---------------------------------------------------------------- */

@implementation RNDISSession {
    IOUSBHostInterface *_control;
    IOUSBHostInterface *_data;
    uint8_t _controlNumber;
    NSMutableData *_controlBuffer;
    uint32_t _nextRequestId;
    uint8_t _mac[6];
    NSMutableData *_txBuffer;
}

+ (void)listInterfaces
{
    for (NSNumber *n in allInterfaces()) {
        const io_service_t s = n.unsignedIntValue;
        io_name_t name = "";
        IORegistryEntryGetName(s, name);
        USB_INFO("  %02X/%02X/%02X  interface %d  %s",
                 intProperty(s, "bInterfaceClass"), intProperty(s, "bInterfaceSubClass"),
                 intProperty(s, "bInterfaceProtocol"), intProperty(s, "bInterfaceNumber"), name);
        IOObjectRelease(s);
    }
}

+ (nullable instancetype)openWithSeize:(BOOL)seize
                           onTerminate:(nullable void (^)(void))onTerminate
{
    io_service_t controlService, dataService;
    if (!findInterfaces(&controlService, &dataService)) {
        if (controlService != IO_OBJECT_NULL)
            IOObjectRelease(controlService);
        USB_FAIL("no RNDIS control interface with a CDC Data interface beside it");
        USB_INFO("is the phone plugged in, with USB tethering on? USB interfaces seen:");
        [self listInterfaces];
        USB_INFO("02/0D/00 is CDC NCM: macOS drives that itself, and this driver is not needed");
        return nil;
    }

    IOUSBHostInterestHandler handler = nil;
    if (onTerminate != nil) {
        handler = ^(IOUSBHostObject *object, uint32_t type, void *argument) {
            if (type == kIOMessageServiceIsTerminated)
                onTerminate();
        };
    }

    RNDISSession *session = [[RNDISSession alloc] init];
    session->_control = openInterface(controlService, "control", seize, handler);
    session->_data = openInterface(dataService, "data", seize, nil);
    IOObjectRelease(controlService);
    IOObjectRelease(dataService);
    if (session->_control == nil || session->_data == nil) {
        [session close];
        return nil;
    }

    session->_controlNumber = session->_control.interfaceDescriptor->bInterfaceNumber;
    session->_controlBuffer = [NSMutableData dataWithLength:RNDIS_CONTROL_BUFFER_SIZE];
    session->_nextRequestId = 1;

    /* Bulk endpoints, found by walking the descriptor: their addresses
     * are not fixed. */
    const IOUSBConfigurationDescriptor *config = session->_data.configurationDescriptor;
    const IOUSBInterfaceDescriptor *desc = session->_data.interfaceDescriptor;
    const IOUSBEndpointDescriptor *ep = NULL;
    uint8_t inAddress = 0, outAddress = 0;
    uint16_t outMaxPacket = 0;

    while ((ep = IOUSBGetNextEndpointDescriptor(config, desc,
                                                (const IOUSBDescriptorHeader *)ep)) != NULL) {
        if (IOUSBGetEndpointType(ep) != kIOUSBEndpointTypeBulk)
            continue;
        if (IOUSBGetEndpointDirection(ep) == kIOUSBEndpointDirectionIn) {
            if (inAddress == 0)
                inAddress = IOUSBGetEndpointAddress(ep);
        } else if (outAddress == 0) {
            outAddress = IOUSBGetEndpointAddress(ep);
            outMaxPacket = OSSwapLittleToHostInt16(ep->wMaxPacketSize) & 0x07FF;
        }
    }
    if (inAddress == 0 || outAddress == 0) {
        USB_FAIL("the data interface has no bulk pair (in 0x%02x, out 0x%02x)",
                 inAddress, outAddress);
        [session close];
        return nil;
    }

    NSError *error = nil;
    session->_inPipe = [session->_data copyPipeWithAddress:inAddress error:&error];
    session->_outPipe = [session->_data copyPipeWithAddress:outAddress error:&error];
    if (session->_inPipe == nil || session->_outPipe == nil) {
        USB_FAIL("copying the bulk pipes: %s", error.localizedDescription.UTF8String);
        [session close];
        return nil;
    }
    USB_OK("bulk in 0x%02x, bulk out 0x%02x", inAddress, outAddress);

    /* 512 at high speed, 1024 at SuperSpeed; never trust a zero. */
    session->_outMaxPacketSize = outMaxPacket != 0 ? outMaxPacket : 512;

    /* One byte over: see the pad in -sendFrame:length:. */
    session->_txBuffer = [NSMutableData dataWithLength:RNDIS_ETHERNET_MAX_FRAME +
                                                        sizeof(struct rndis_packet_msg) + 1];
    return session;
}

- (const uint8_t *)mac
{
    return _mac;
}

/* ---- the control channel ---- */

- (BOOL)sendControl:(uint32_t)length
{
    IOUSBDeviceRequest request = {
        .bmRequestType = kIOUSBDeviceRequestDirectionOut |
                         kIOUSBDeviceRequestTypeClass |
                         kIOUSBDeviceRequestRecipientInterface,
        .bRequest = RNDIS_SEND_ENCAPSULATED_COMMAND,
        .wValue = 0,
        .wIndex = _controlNumber,
        .wLength = (uint16_t)length,
    };
    NSError *error = nil;
    NSUInteger transferred = 0;

    if (![_control sendDeviceRequest:request
                                data:_controlBuffer
                    bytesTransferred:&transferred
                   completionTimeout:CONTROL_TIMEOUT_S
                               error:&error]) {
        USB_FAIL("SEND_ENCAPSULATED_COMMAND: %s (0x%08lx)",
                 error.localizedDescription.UTF8String, (long)error.code);
        return NO;
    }
    return YES;
}

/* A device with nothing to say answers with zero bytes: "not yet". */
- (BOOL)readControl:(uint32_t *)length
{
    *length = 0;

    for (int attempt = 0; attempt < CONTROL_POLL_ATTEMPTS; attempt++) {
        IOUSBDeviceRequest request = {
            .bmRequestType = kIOUSBDeviceRequestDirectionIn |
                             kIOUSBDeviceRequestTypeClass |
                             kIOUSBDeviceRequestRecipientInterface,
            .bRequest = RNDIS_GET_ENCAPSULATED_RESPONSE,
            .wValue = 0,
            .wIndex = _controlNumber,
            .wLength = (uint16_t)RNDIS_CONTROL_BUFFER_SIZE,
        };
        NSError *error = nil;
        NSUInteger transferred = 0;

        if (![_control sendDeviceRequest:request
                                    data:_controlBuffer
                        bytesTransferred:&transferred
                       completionTimeout:CONTROL_TIMEOUT_S
                                   error:&error]) {
            USB_FAIL("GET_ENCAPSULATED_RESPONSE: %s (0x%08lx)",
                     error.localizedDescription.UTF8String, (long)error.code);
            return NO;
        }
        if (transferred > 0) {
            *length = (uint32_t)transferred;
            if (attempt > 0)
                USB_INFO("answered after %d polls", attempt + 1);
            return YES;
        }
        usleep(CONTROL_POLL_MS * 1000);
    }

    USB_FAIL("the device never answered (%d polls)", CONTROL_POLL_ATTEMPTS);
    return NO;
}

- (BOOL)initialize
{
    uint8_t *bytes = _controlBuffer.mutableBytes;
    uint32_t replyLength = 0;

    const uint32_t initId = _nextRequestId++;
    size_t length = rndis_build_initialize(bytes, RNDIS_CONTROL_BUFFER_SIZE,
                                           initId, RNDIS_MAX_TRANSFER_SIZE);
    if (![self sendControl:(uint32_t)length] || ![self readControl:&replyLength])
        return NO;

    rndis_reply_result_t result =
        rndis_parse_initialize_reply(bytes, replyLength, initId, &_initReply);
    if (result != RNDIS_REPLY_OK) {
        USB_FAIL("INITIALIZE refused: %s", rndis_reply_result_name(result));
        hexdump(bytes, replyLength);
        return NO;
    }
    _maxTransfer = _initReply.max_transfer_size;
    USB_OK("INITIALIZE: RNDIS %u.%u, max transfer %u, %u packets per transfer, alignment 2^%u",
           _initReply.major_version, _initReply.minor_version, _maxTransfer,
           _initReply.max_packets_per_transfer, _initReply.packet_alignment_factor);
    if (_maxTransfer == 0 || _maxTransfer > RNDIS_MAX_TRANSFER_SIZE) {
        USB_INFO("clamping max transfer to %u, as the extension does", RNDIS_MAX_TRANSFER_SIZE);
        _maxTransfer = RNDIS_MAX_TRANSFER_SIZE;
    }

    const uint32_t macId = _nextRequestId++;
    length = rndis_build_query(bytes, RNDIS_CONTROL_BUFFER_SIZE, macId,
                               OID_802_3_PERMANENT_ADDRESS);
    if (![self sendControl:(uint32_t)length] || ![self readControl:&replyLength])
        return NO;

    const uint8_t *info = NULL;
    uint32_t infoLength = 0;
    result = rndis_parse_query_reply(bytes, replyLength, macId, &info, &infoLength);
    if (result != RNDIS_REPLY_OK || infoLength < RNDIS_ETHERNET_ADDR_LEN) {
        USB_FAIL("OID_802_3_PERMANENT_ADDRESS: %s, %u bytes",
                 rndis_reply_result_name(result), infoLength);
        hexdump(bytes, replyLength);
        return NO;
    }
    memcpy(_mac, info, sizeof(_mac));
    USB_OK("MAC %02x:%02x:%02x:%02x:%02x:%02x",
           _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
    return YES;
}

- (void)printQueryU32:(uint32_t)oid name:(const char *)name
{
    uint8_t *bytes = _controlBuffer.mutableBytes;
    const uint32_t id = _nextRequestId++;
    const size_t length = rndis_build_query(bytes, RNDIS_CONTROL_BUFFER_SIZE, id, oid);
    uint32_t replyLength = 0;

    if (length == 0 || ![self sendControl:(uint32_t)length] || ![self readControl:&replyLength])
        return;

    const uint8_t *info = NULL;
    uint32_t infoLength = 0;
    const rndis_reply_result_t result =
        rndis_parse_query_reply(bytes, replyLength, id, &info, &infoLength);
    if (result != RNDIS_REPLY_OK || infoLength < 4) {
        USB_INFO("%s: not answered (%s, %u bytes)", name, rndis_reply_result_name(result),
                 infoLength);
        return;
    }
    uint32_t value;
    memcpy(&value, info, sizeof(value));
    USB_OK("%s = %u", name, rndis_le32_to_cpu(value));
}

- (BOOL)setPacketFilter:(uint32_t)filter
{
    uint8_t *bytes = _controlBuffer.mutableBytes;
    uint32_t replyLength = 0;
    const uint32_t id = _nextRequestId++;
    const size_t length = rndis_build_set_u32(bytes, RNDIS_CONTROL_BUFFER_SIZE, id,
                                              OID_GEN_CURRENT_PACKET_FILTER, filter);
    if (![self sendControl:(uint32_t)length] || ![self readControl:&replyLength])
        return NO;

    const rndis_reply_result_t result =
        rndis_check_completion(bytes, replyLength, RNDIS_MSG_SET, id);
    if (result != RNDIS_REPLY_OK) {
        USB_FAIL("packet filter 0x%08x refused: %s", filter, rndis_reply_result_name(result));
        hexdump(bytes, replyLength);
        return NO;
    }
    USB_OK("packet filter 0x%08x", filter);
    return YES;
}

- (void)halt
{
    if (_control == nil)
        return;
    const size_t length = rndis_build_halt(_controlBuffer.mutableBytes,
                                           RNDIS_CONTROL_BUFFER_SIZE, _nextRequestId++);
    if ([self sendControl:(uint32_t)length])
        USB_OK("HALT sent");
}

/* ---- data ---- */

- (BOOL)sendFrame:(const uint8_t *)frame length:(size_t)length
{
    const size_t headerLength = sizeof(struct rndis_packet_msg);
    uint8_t *tx = _txBuffer.mutableBytes;

    if (length > RNDIS_ETHERNET_MAX_FRAME ||
        rndis_build_packet_header(tx, headerLength + length, (uint32_t)length) != headerLength)
        return NO;
    memcpy(tx + headerLength, frame, length);

    /*
     * A transfer that is a whole number of USB packets does not end, as
     * far as the device can tell: it waits for a short packet, and glues
     * the next transfer onto this one. One byte past the record makes the
     * last packet short. It is outside msg_len, and RNDIS devices expect
     * it -- Windows and Linux hosts both do the same.
     */
    size_t total = headerLength + length;
    if (total % _outMaxPacketSize == 0)
        tx[total++] = 0;

    /*
     * A transfer's size is its NSData's length, so send exactly the record
     * -- a view onto the front of the buffer, not the whole of it.
     */
    NSMutableData *record = [NSMutableData dataWithBytesNoCopy:tx
                                                        length:total
                                                  freeWhenDone:NO];
    NSError *error = nil;
    NSUInteger sent = 0;
    if (![_outPipe sendIORequestWithData:record
                        bytesTransferred:&sent
                       completionTimeout:BULK_OUT_TIMEOUT_S
                                   error:&error])
        return NO;
    return sent == total;
}

- (void)close
{
    [_inPipe abortWithError:nil];
    [_outPipe abortWithError:nil];
    _inPipe = nil;
    _outPipe = nil;
    [_data destroy];
    [_control destroy];
    _data = nil;
    _control = nil;
}

@end
