/*
 * AndroidTetherDriver.cpp -- lifecycle, USB bring-up and the RNDIS
 * control channel.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * The call sequence in Start follows Apple's NetworkingDriverKit sample
 * ("Connecting a network driver"): one dispatch queue, then the buffer
 * pool, then the four packet queues, then RegisterEthernetInterface, then
 * the media types and link status, then RegisterService.
 *
 * What is ours is everything between: an Android phone exposes RNDIS over
 * two USB interfaces, and the one that matches here is the control
 * interface. The bulk endpoints live on the other one, which has to be
 * found on the same device and opened separately.
 */

#include <os/log.h>
#include <stdio.h>
#include <string.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/OSData.h>
#include <USBDriverKit/AppleUSBDescriptorParsing.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/USBDriverKitDefs.h>

#include "AndroidTetherDriver.h"
#include "AndroidTetherDriver_ivars.h"
#include "rndis_control.h"
#include "rndis_frame.h"

#define ATDLog(fmt, ...) os_log(OS_LOG_DEFAULT, "AndroidTether: " fmt, ##__VA_ARGS__)

/*
 * USB class codes for the data interface of an RNDIS function. The
 * control interface is matched by the personalities in Info.plist; the
 * data interface is always CDC Data, and on every Android gadget seen so
 * far it is the next interface along.
 */
#define USB_CLASS_CDC_DATA 0x0A

/*
 * How long to wait for the device to produce a control response, and how
 * often to ask. RNDIS signals a waiting response on an interrupt
 * endpoint, but polling the control endpoint is allowed and is what the
 * handshake needs: it runs once, at Start, and a few milliseconds there
 * is cheaper than another endpoint and another callback path.
 */
#define ATD_CONTROL_POLL_MS       5
#define ATD_CONTROL_POLL_ATTEMPTS 200   /* one second in total */

/* Control transfers are not on a hot path; a generous timeout avoids
 * failing a handshake on a phone that is busy unlocking. */
#define ATD_CONTROL_TIMEOUT_MS 5000

/* ---------------------------------------------------------------- */

bool
AndroidTetherDriver::init()
{
    if (!super::init())
        return false;

    ivars = IONewZero(AndroidTetherDriver_IVars, 1);
    if (ivars == nullptr)
        return false;

    ivars->nextRequestId = 1;
    return true;
}

void
AndroidTetherDriver::free()
{
    if (ivars != nullptr)
        IOSafeDeleteNULL(ivars, AndroidTetherDriver_IVars, 1);
    super::free();
}

/* ---------------------------------------------------------------- */

/*
 * Walk the device's interfaces for the CDC Data interface that belongs to
 * this RNDIS function, open it, and take its two bulk pipes.
 *
 * The control interface we were matched against carries no bulk
 * endpoints, so without this there is nowhere for packets to go.
 */
kern_return_t
AndroidTetherDriver::OpenDataInterface()
{
    kern_return_t ret;
    uintptr_t iterator = 0;
    bool iteratorValid = false;

    ret = ivars->controlInterface->CopyDevice(&ivars->device);
    if (ret != kIOReturnSuccess) {
        ATDLog("CopyDevice failed: 0x%08x", ret);
        return ret;
    }

    ret = ivars->device->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) {
        ATDLog("opening the device failed: 0x%08x", ret);
        return ret;
    }
    ivars->deviceOpen = true;

    ret = ivars->device->CreateInterfaceIterator(&iterator);
    if (ret != kIOReturnSuccess) {
        ATDLog("CreateInterfaceIterator failed: 0x%08x", ret);
        return ret;
    }
    iteratorValid = true;

    for (;;) {
        IOUSBHostInterface *candidate = nullptr;

        ret = ivars->device->CopyInterface(iterator, &candidate);
        if (ret != kIOReturnSuccess || candidate == nullptr)
            break;

        const IOUSBConfigurationDescriptor *config =
            candidate->CopyConfigurationDescriptor();
        const IOUSBInterfaceDescriptor *desc =
            (config != nullptr) ? candidate->GetInterfaceDescriptor(config) : nullptr;

        if (desc != nullptr && desc->bInterfaceClass == USB_CLASS_CDC_DATA &&
            ivars->dataInterface == nullptr) {
            ivars->dataInterface = candidate;    /* keep the reference */
            candidate = nullptr;
        }

        if (config != nullptr)
            IOUSBHostFreeDescriptor(config);
        OSSafeReleaseNULL(candidate);

        if (ivars->dataInterface != nullptr)
            break;
    }

    if (iteratorValid)
        ivars->device->DestroyInterfaceIterator(iterator);

    if (ivars->dataInterface == nullptr) {
        ATDLog("no CDC Data interface on this device");
        return kIOReturnNotFound;
    }

    ret = ivars->dataInterface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        ATDLog("opening the data interface failed: 0x%08x", ret);
        return ret;
    }
    ivars->dataInterfaceOpen = true;

    /*
     * Find the bulk endpoints by walking the configuration descriptor.
     * Their addresses are not fixed: assuming 0x81 and 0x01 works on most
     * phones and fails on exactly the ones worth supporting.
     */
    const IOUSBConfigurationDescriptor *config =
        ivars->dataInterface->CopyConfigurationDescriptor();
    if (config == nullptr) {
        ATDLog("no configuration descriptor for the data interface");
        return kIOReturnNotFound;
    }

    const IOUSBInterfaceDescriptor *desc =
        ivars->dataInterface->GetInterfaceDescriptor(config);
    const IOUSBEndpointDescriptor *endpoint = nullptr;
    uint8_t inAddress = 0, outAddress = 0;

    while ((endpoint = IOUSBGetNextEndpointDescriptor(
                config, desc, (const IOUSBDescriptorHeader *)endpoint)) != nullptr) {
        if (IOUSBGetEndpointType(endpoint) != kIOUSBEndpointTypeBulk)
            continue;

        const uint8_t address = IOUSBGetEndpointAddress(endpoint);
        if (IOUSBGetEndpointDirection(endpoint) == kIOUSBEndpointDirectionIn) {
            if (inAddress == 0)
                inAddress = address;
        } else if (outAddress == 0) {
            outAddress = address;
        }
    }

    IOUSBHostFreeDescriptor(config);

    if (inAddress == 0 || outAddress == 0) {
        ATDLog("the data interface has no bulk pair (in 0x%02x, out 0x%02x)",
               inAddress, outAddress);
        return kIOReturnNotFound;
    }

    ret = ivars->dataInterface->CopyPipe(inAddress, &ivars->inPipe);
    if (ret != kIOReturnSuccess) {
        ATDLog("CopyPipe(in) failed: 0x%08x", ret);
        return ret;
    }

    ret = ivars->dataInterface->CopyPipe(outAddress, &ivars->outPipe);
    if (ret != kIOReturnSuccess) {
        ATDLog("CopyPipe(out) failed: 0x%08x", ret);
        return ret;
    }

    return kIOReturnSuccess;
}

/* ---- the control channel ---- */

/*
 * SEND_ENCAPSULATED_COMMAND: the RNDIS message in the control buffer goes
 * to the device over endpoint 0, addressed to the control interface.
 */
kern_return_t
AndroidTetherDriver::SendControlMessage(uint32_t length)
{
    uint16_t transferred = 0;

    const kern_return_t ret = ivars->controlInterface->DeviceRequest(
        /* bmRequestType */ kIOUSBDeviceRequestDirectionOut |
                            kIOUSBDeviceRequestTypeClass |
                            kIOUSBDeviceRequestRecipientInterface,
        RNDIS_SEND_ENCAPSULATED_COMMAND,
        /* wValue */ 0,
        /* wIndex */ ivars->controlInterfaceNumber,
        (uint16_t)length,
        ivars->controlBuffer,
        &transferred,
        ATD_CONTROL_TIMEOUT_MS);

    if (ret != kIOReturnSuccess)
        ATDLog("sending a control message failed: 0x%08x", ret);

    return ret;
}

/*
 * GET_ENCAPSULATED_RESPONSE, retried until the device has an answer.
 *
 * A device with nothing to say answers with zero bytes rather than an
 * error, so a single request that returns empty is not a failure -- it is
 * "not yet". Treating it as a failure is how a handshake ends up racing
 * the phone's own startup.
 */
kern_return_t
AndroidTetherDriver::ReadControlResponse(uint32_t *length)
{
    *length = 0;

    for (uint32_t attempt = 0; attempt < ATD_CONTROL_POLL_ATTEMPTS; attempt++) {
        uint16_t transferred = 0;

        const kern_return_t ret = ivars->controlInterface->DeviceRequest(
            kIOUSBDeviceRequestDirectionIn |
            kIOUSBDeviceRequestTypeClass |
            kIOUSBDeviceRequestRecipientInterface,
            RNDIS_GET_ENCAPSULATED_RESPONSE,
            /* wValue */ 0,
            /* wIndex */ ivars->controlInterfaceNumber,
            (uint16_t)RNDIS_CONTROL_BUFFER_SIZE,
            ivars->controlBuffer,
            &transferred,
            ATD_CONTROL_TIMEOUT_MS);

        if (ret != kIOReturnSuccess) {
            ATDLog("reading a control response failed: 0x%08x", ret);
            return ret;
        }

        if (transferred > 0) {
            *length = transferred;
            return kIOReturnSuccess;
        }

        IOSleep(ATD_CONTROL_POLL_MS);
    }

    ATDLog("the device never answered a control message");
    return kIOReturnTimeout;
}

/*
 * REMOTE_NDIS_INITIALIZE, then OID_802_3_PERMANENT_ADDRESS.
 *
 * The packet filter is deliberately not set here: it belongs to
 * SetInterfaceEnable, so a phone that is plugged in but whose interface
 * the user has not brought up is not asked to forward traffic.
 */
kern_return_t
AndroidTetherDriver::PerformHandshake(IOUserNetworkMACAddress *mac)
{
    kern_return_t ret;
    uint32_t replyLength = 0;

    /* ---- INITIALIZE ---- */
    const uint32_t initId = ivars->nextRequestId++;
    size_t length = rndis_build_initialize(ivars->controlBytes,
                                           RNDIS_CONTROL_BUFFER_SIZE,
                                           initId, RNDIS_MAX_TRANSFER_SIZE);
    if (length == 0)
        return kIOReturnNoMemory;

    ret = SendControlMessage((uint32_t)length);
    if (ret != kIOReturnSuccess)
        return ret;

    ret = ReadControlResponse(&replyLength);
    if (ret != kIOReturnSuccess)
        return ret;

    rndis_init_reply_t init;
    rndis_reply_result_t result =
        rndis_parse_initialize_reply(ivars->controlBytes, replyLength, initId, &init);
    if (result != RNDIS_REPLY_OK) {
        ATDLog("the device refused INITIALIZE: %s", rndis_reply_result_name(result));
        return kIOReturnNotResponding;
    }

    /*
     * Believe the device's limit only when it is smaller than ours. A
     * device claiming a larger transfer than we allocated a buffer for is
     * not an opportunity, it is an overrun waiting for a busy phone.
     */
    ivars->deviceMaxTransferSize = init.max_transfer_size;
    if (ivars->deviceMaxTransferSize == 0 ||
        ivars->deviceMaxTransferSize > RNDIS_MAX_TRANSFER_SIZE)
        ivars->deviceMaxTransferSize = RNDIS_MAX_TRANSFER_SIZE;

    ATDLog("RNDIS %u.%u, max transfer %u, up to %u packets per transfer",
           init.major_version, init.minor_version,
           ivars->deviceMaxTransferSize, init.max_packets_per_transfer);

    /* ---- the MAC address ---- */
    const uint32_t macId = ivars->nextRequestId++;
    length = rndis_build_query(ivars->controlBytes, RNDIS_CONTROL_BUFFER_SIZE,
                               macId, OID_802_3_PERMANENT_ADDRESS);
    if (length == 0)
        return kIOReturnNoMemory;

    ret = SendControlMessage((uint32_t)length);
    if (ret != kIOReturnSuccess)
        return ret;

    ret = ReadControlResponse(&replyLength);
    if (ret != kIOReturnSuccess)
        return ret;

    const uint8_t *info = nullptr;
    uint32_t infoLength = 0;
    result = rndis_parse_query_reply(ivars->controlBytes, replyLength, macId,
                                     &info, &infoLength);
    if (result != RNDIS_REPLY_OK || infoLength < RNDIS_ETHERNET_ADDR_LEN) {
        ATDLog("could not read the device's MAC address: %s",
               rndis_reply_result_name(result));
        return kIOReturnNotResponding;
    }

    memcpy(mac->octet, info, RNDIS_ETHERNET_ADDR_LEN);
    ATDLog("MAC address %02x:%02x:%02x:%02x:%02x:%02x",
           mac->octet[0], mac->octet[1], mac->octet[2],
           mac->octet[3], mac->octet[4], mac->octet[5]);

    return kIOReturnSuccess;
}

/*
 * OID_GEN_CURRENT_PACKET_FILTER. Zero tells the device to forward
 * nothing, which is how the interface is taken down without unplugging.
 */
kern_return_t
AndroidTetherDriver::SetPacketFilter(uint32_t filter)
{
    const uint32_t id = ivars->nextRequestId++;

    const size_t length = rndis_build_set_u32(ivars->controlBytes,
                                              RNDIS_CONTROL_BUFFER_SIZE, id,
                                              OID_GEN_CURRENT_PACKET_FILTER,
                                              filter);
    if (length == 0)
        return kIOReturnNoMemory;

    kern_return_t ret = SendControlMessage((uint32_t)length);
    if (ret != kIOReturnSuccess)
        return ret;

    uint32_t replyLength = 0;
    ret = ReadControlResponse(&replyLength);
    if (ret != kIOReturnSuccess)
        return ret;

    const rndis_reply_result_t result =
        rndis_check_completion(ivars->controlBytes, replyLength, RNDIS_MSG_SET, id);
    if (result != RNDIS_REPLY_OK) {
        ATDLog("the device refused the packet filter 0x%08x: %s", filter,
               rndis_reply_result_name(result));
        return kIOReturnNotResponding;
    }

    return kIOReturnSuccess;
}

/* ---------------------------------------------------------------- */

kern_return_t
IMPL(AndroidTetherDriver, Start)
{
    kern_return_t ret;
    IOAddressSegment segment = {};
    IOUserNetworkMACAddress mac = {};

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        ATDLog("super::Start failed: 0x%08x", ret);
        return ret;
    }

    ivars->controlInterface = OSDynamicCast(IOUSBHostInterface, provider);
    if (ivars->controlInterface == nullptr) {
        ATDLog("provider is not a USB interface");
        ret = kIOReturnNoDevice;
        goto fail;
    }

    /*
     * One queue for everything. Every callback this driver registers --
     * both pipe completions, the transmit doorbell and the retry timer --
     * is delivered here, which is what lets the ivars be touched without
     * locking.
     */
    ret = CopyDispatchQueue(kIOServiceDefaultQueueName, &ivars->dispatchQueue);
    if (ret != kIOReturnSuccess) {
        ATDLog("CopyDispatchQueue failed: 0x%08x", ret);
        goto fail;
    }

    ret = ivars->controlInterface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        ATDLog("opening the control interface failed: 0x%08x", ret);
        goto fail;
    }
    ivars->controlInterfaceOpen = true;

    {
        const IOUSBConfigurationDescriptor *config =
            ivars->controlInterface->CopyConfigurationDescriptor();
        const IOUSBInterfaceDescriptor *desc =
            (config != nullptr)
                ? ivars->controlInterface->GetInterfaceDescriptor(config)
                : nullptr;
        if (desc == nullptr) {
            ATDLog("no interface descriptor for the control interface");
            if (config != nullptr)
                IOUSBHostFreeDescriptor(config);
            ret = kIOReturnNoDevice;
            goto fail;
        }
        ivars->controlInterfaceNumber = desc->bInterfaceNumber;
        IOUSBHostFreeDescriptor(config);
    }

    ret = ivars->controlInterface->CreateIOBuffer(kIOMemoryDirectionInOut,
                                                  RNDIS_CONTROL_BUFFER_SIZE,
                                                  &ivars->controlBuffer);
    if (ret != kIOReturnSuccess) {
        ATDLog("allocating the control buffer failed: 0x%08x", ret);
        goto fail;
    }

    ret = ivars->controlBuffer->GetAddressRange(&segment);
    if (ret != kIOReturnSuccess)
        goto fail;
    ivars->controlBytes = (uint8_t *)segment.address;

    ret = OpenDataInterface();
    if (ret != kIOReturnSuccess)
        goto fail;

    ret = PerformHandshake(&mac);
    if (ret != kIOReturnSuccess)
        goto fail;

    /* ---- transfer buffers, sized by what the device agreed to ---- */

    ivars->rxBufferSize = ivars->deviceMaxTransferSize;
    ivars->txBufferSize = (uint32_t)(RNDIS_ETHERNET_MAX_FRAME +
                                     sizeof(struct rndis_packet_msg));

    for (uint32_t i = 0; i < ATD_RX_SLOTS; i++) {
        ret = ivars->dataInterface->CreateIOBuffer(kIOMemoryDirectionIn,
                                                   ivars->rxBufferSize,
                                                   &ivars->rxBuffer[i]);
        if (ret != kIOReturnSuccess)
            goto fail;

        ret = ivars->rxBuffer[i]->GetAddressRange(&segment);
        if (ret != kIOReturnSuccess)
            goto fail;
        ivars->rxBytes[i] = (uint8_t *)segment.address;

        /*
         * The action's reference holds the slot index, which is how a
         * completion knows which buffer it is about. Without it the
         * driver would need one callback method per slot.
         */
        ret = CreateActionReadComplete(sizeof(uint64_t), &ivars->rxAction[i]);
        if (ret != kIOReturnSuccess)
            goto fail;
        *(uint64_t *)ivars->rxAction[i]->GetReference() = i;
    }

    for (uint32_t i = 0; i < ATD_TX_SLOTS; i++) {
        ret = ivars->dataInterface->CreateIOBuffer(kIOMemoryDirectionOut,
                                                   ivars->txBufferSize,
                                                   &ivars->txBuffer[i]);
        if (ret != kIOReturnSuccess)
            goto fail;

        ret = ivars->txBuffer[i]->GetAddressRange(&segment);
        if (ret != kIOReturnSuccess)
            goto fail;
        ivars->txBytes[i] = (uint8_t *)segment.address;

        ret = CreateActionWriteComplete(sizeof(uint64_t), &ivars->txAction[i]);
        if (ret != kIOReturnSuccess)
            goto fail;
        *(uint64_t *)ivars->txAction[i]->GetReference() = i;
    }

    /* ---- the retry timer ---- */

    ret = IOTimerDispatchSource::Create(ivars->dispatchQueue, &ivars->retryTimer);
    if (ret != kIOReturnSuccess)
        goto fail;

    ret = CreateActionRetryTimer(0, &ivars->retryTimerAction);
    if (ret != kIOReturnSuccess)
        goto fail;

    ret = ivars->retryTimer->SetHandler(ivars->retryTimerAction);
    if (ret != kIOReturnSuccess)
        goto fail;

    /* ---- the packet pool and the four queues ---- */

    ret = IOUserNetworkPacketBufferPool::Create(this, "AndroidTether",
                                                ATD_POOL_PACKETS,
                                                ATD_POOL_BUFFERS,
                                                ATD_POOL_BUFFER_SIZE,
                                                &ivars->pool);
    if (ret != kIOReturnSuccess) {
        ATDLog("creating the packet pool failed: 0x%08x", ret);
        goto fail;
    }

    ret = CreateActionTxPacketAvailable(0, &ivars->txDoorbellAction);
    if (ret != kIOReturnSuccess)
        goto fail;

    ret = IOUserNetworkTxSubmissionQueue::Create(ivars->pool, this,
                                                 ATD_QUEUE_CAPACITY, 0,
                                                 ivars->dispatchQueue,
                                                 &ivars->txSubmission);
    if (ret != kIOReturnSuccess)
        goto fail;

    {
        IODataQueueDispatchSource *source = nullptr;

        ret = ivars->txSubmission->CopyDataQueue(&source);
        if (ret != kIOReturnSuccess)
            goto fail;

        ret = source->SetDataAvailableHandler(ivars->txDoorbellAction);
        OSSafeReleaseNULL(source);
        if (ret != kIOReturnSuccess)
            goto fail;
    }

    ret = IOUserNetworkTxCompletionQueue::Create(ivars->pool, this,
                                                 ATD_QUEUE_CAPACITY, 0,
                                                 ivars->dispatchQueue,
                                                 &ivars->txCompletion);
    if (ret != kIOReturnSuccess)
        goto fail;

    ret = IOUserNetworkRxSubmissionQueue::Create(ivars->pool, this,
                                                 ATD_QUEUE_CAPACITY, 0,
                                                 ivars->dispatchQueue,
                                                 &ivars->rxSubmission);
    if (ret != kIOReturnSuccess)
        goto fail;

    ret = IOUserNetworkRxCompletionQueue::Create(ivars->pool, this,
                                                 ATD_QUEUE_CAPACITY, 0,
                                                 ivars->dispatchQueue,
                                                 &ivars->rxCompletion);
    if (ret != kIOReturnSuccess)
        goto fail;

    {
        IOUserNetworkPacketQueue *queues[4] = {
            ivars->rxSubmission,
            ivars->rxCompletion,
            ivars->txSubmission,
            ivars->txCompletion,
        };

        ret = RegisterEthernetInterface(mac, ivars->pool, queues, 4);
        if (ret != kIOReturnSuccess) {
            ATDLog("RegisterEthernetInterface failed: 0x%08x", ret);
            goto fail;
        }
    }

    /*
     * USB gives no link speed for the tether itself, and the figure is
     * cosmetic: it is what `ifconfig` prints, not anything the stack
     * schedules against. 480 Mb/s is the honest ceiling of the USB 2.0
     * link every Android RNDIS gadget presents.
     */
    {
        const IOUserNetworkMediaType media =
            kIOUserNetworkMediaEthernet100BaseTX | kIOUserNetworkMediaOptionFullDuplex;

        ret = ReportAvailableMediaTypes(&media, 1);
        if (ret != kIOReturnSuccess)
            goto fail;

        ret = SelectMediaType(media);
        if (ret != kIOReturnSuccess)
            goto fail;

        ret = ReportLinkStatus(kIOUserNetworkLinkStatusActive, media);
        if (ret != kIOReturnSuccess)
            goto fail;
    }

    RegisterService();

    ATDLog("started on interface %u", ivars->controlInterfaceNumber);
    return kIOReturnSuccess;

fail:
    TearDown();
    Stop(provider, SUPERDISPATCH);
    return (ret == kIOReturnSuccess) ? kIOReturnError : ret;
}

/* ---------------------------------------------------------------- */

kern_return_t
IMPL(AndroidTetherDriver, SetInterfaceEnable)
{
    ATDLog("interface %s", isEnable ? "up" : "down");

    if (isEnable) {
        if (ivars->interfaceEnabled)
            return kIOReturnSuccess;

        kern_return_t ret = SetPacketFilter(RNDIS_FILTER_DEFAULT);
        if (ret != kIOReturnSuccess)
            return ret;

        ivars->interfaceEnabled = true;

        ivars->rxSubmission->SetEnable(true);
        ivars->rxCompletion->SetEnable(true);
        ivars->txSubmission->SetEnable(true);
        ivars->txCompletion->SetEnable(true);

        SubmitReads();
        return kIOReturnSuccess;
    }

    if (!ivars->interfaceEnabled)
        return kIOReturnSuccess;

    ivars->interfaceEnabled = false;

    /*
     * Order matters on the way down: tell the device to stop sending
     * before abandoning the transfers that would have received what it
     * sends, or the last few arrive against a pipe nobody is reading.
     */
    SetPacketFilter(0);

    if (ivars->inPipe != nullptr)
        ivars->inPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, this);
    if (ivars->outPipe != nullptr)
        ivars->outPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, this);

    ivars->txSubmission->SetEnable(false);
    ivars->txCompletion->SetEnable(false);
    ivars->rxSubmission->SetEnable(false);
    ivars->rxCompletion->SetEnable(false);

    return kIOReturnSuccess;
}

/* ---------------------------------------------------------------- */

/*
 * Release in the reverse of the order Start acquired, and only what was
 * actually acquired: Stop can run after a Start that failed halfway.
 */
void
AndroidTetherDriver::TearDown()
{
    ivars->stopping = true;

    if (ivars->retryTimer != nullptr)
        ivars->retryTimer->Cancel(nullptr);

    if (ivars->inPipe != nullptr)
        ivars->inPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, this);
    if (ivars->outPipe != nullptr)
        ivars->outPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, this);

    OSSafeReleaseNULL(ivars->rxSubmission);
    OSSafeReleaseNULL(ivars->rxCompletion);
    OSSafeReleaseNULL(ivars->txSubmission);
    OSSafeReleaseNULL(ivars->txCompletion);
    OSSafeReleaseNULL(ivars->pool);

    OSSafeReleaseNULL(ivars->txDoorbellAction);
    OSSafeReleaseNULL(ivars->retryTimerAction);
    OSSafeReleaseNULL(ivars->retryTimer);

    for (uint32_t i = 0; i < ATD_RX_SLOTS; i++) {
        OSSafeReleaseNULL(ivars->rxAction[i]);
        OSSafeReleaseNULL(ivars->rxBuffer[i]);
        ivars->rxBytes[i] = nullptr;
    }

    for (uint32_t i = 0; i < ATD_TX_SLOTS; i++) {
        OSSafeReleaseNULL(ivars->txAction[i]);
        OSSafeReleaseNULL(ivars->txBuffer[i]);
        ivars->txBytes[i] = nullptr;
    }

    OSSafeReleaseNULL(ivars->inPipe);
    OSSafeReleaseNULL(ivars->outPipe);

    if (ivars->dataInterfaceOpen) {
        ivars->dataInterface->Close(this, 0);
        ivars->dataInterfaceOpen = false;
    }
    OSSafeReleaseNULL(ivars->dataInterface);

    if (ivars->deviceOpen) {
        ivars->device->Close(this, 0);
        ivars->deviceOpen = false;
    }
    OSSafeReleaseNULL(ivars->device);

    OSSafeReleaseNULL(ivars->controlBuffer);
    ivars->controlBytes = nullptr;

    if (ivars->controlInterfaceOpen) {
        ivars->controlInterface->Close(this, 0);
        ivars->controlInterfaceOpen = false;
    }
    ivars->controlInterface = nullptr;   /* the provider; not ours to release */

    OSSafeReleaseNULL(ivars->dispatchQueue);
}

kern_return_t
IMPL(AndroidTetherDriver, Stop)
{
    ATDLog("stopping: rx %llu frames (%llu dropped), tx %llu frames (%llu dropped)",
           ivars->rxFrames, ivars->rxDropped, ivars->txFrames, ivars->txDropped);

    /*
     * HALT is best-effort. The usual reason Stop runs is that the cable
     * came out, and then there is nothing left to tell.
     */
    if (ivars->controlInterfaceOpen && ivars->controlBytes != nullptr) {
        const size_t length = rndis_build_halt(ivars->controlBytes,
                                               RNDIS_CONTROL_BUFFER_SIZE,
                                               ivars->nextRequestId++);
        if (length > 0)
            SendControlMessage((uint32_t)length);
    }

    TearDown();

    return Stop(provider, SUPERDISPATCH);
}
