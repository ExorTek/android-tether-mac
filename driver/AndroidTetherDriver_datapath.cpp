/*
 * AndroidTetherDriver_datapath.cpp -- frames in both directions.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * The shape of the packet path follows Apple's NetworkingDriverKit
 * sample: dequeue from a submission queue, do the work, enqueue to the
 * matching completion queue, and hand anything that failed back to the
 * pool rather than leaking it.
 *
 * What is RNDIS-specific is only the framing, and none of that is decided
 * here -- `rndis_parse_transfer` and `rndis_build_packet_header` do it,
 * in plain C, under test. This file moves bytes.
 *
 * Everything in it runs on the driver's single dispatch queue.
 */

#include <os/log.h>
#include <string.h>

#include <DriverKit/IOLib.h>
#include <USBDriverKit/USBDriverKitDefs.h>

#include "AndroidTetherDriver.h"
#include "AndroidTetherDriver_ivars.h"
#include "rndis_control.h"
#include "rndis_frame.h"

#define ATDLog(fmt, ...) os_log(OS_LOG_DEFAULT, "AndroidTether: " fmt, ##__VA_ARGS__)

/*
 * How long to wait before re-arming reads after the pipe refused them.
 * Short enough that a user does not notice, long enough that a device in
 * a bad state is not spun against.
 */
#define ATD_RETRY_DELAY_NS (100ULL * 1000ULL * 1000ULL)   /* 100 ms */

/* A bulk IN transfer has no deadline: the phone sends when it has
 * something to send, and a timeout would simply cancel an idle read. */
#define ATD_BULK_IN_TIMEOUT_MS 0

/* A transmit that has not completed in two seconds is not going to. */
#define ATD_BULK_OUT_TIMEOUT_MS 2000

/* ---------------------------------------------------------------- */

/*
 * Give every free receive slot back to the bulk IN pipe.
 *
 * Reads are submitted in a batch rather than one at a time so the device
 * always has somewhere to put the next transfer while an earlier one is
 * being parsed.
 */
void
AndroidTetherDriver::SubmitReads()
{
    if (ivars->stopping || !ivars->interfaceEnabled || ivars->inPipe == nullptr)
        return;

    bool anyRefused = false;

    for (uint32_t i = 0; i < ATD_RX_SLOTS; i++) {
        if (ivars->rxSlotBusy[i])
            continue;

        const kern_return_t ret = ivars->inPipe->AsyncIO(ivars->rxBuffer[i],
                                                         ivars->rxBufferSize,
                                                         ivars->rxAction[i],
                                                         ATD_BULK_IN_TIMEOUT_MS);
        if (ret == kIOReturnSuccess) {
            ivars->rxSlotBusy[i] = true;
            continue;
        }

        ATDLog("submitting a read failed: 0x%08x", ret);
        anyRefused = true;
        break;
    }

    /*
     * If the pipe refused every slot there is no completion left to
     * restart the flow, and the interface would go quiet for good. The
     * timer is the only thing standing between that and a reconnect.
     */
    if (anyRefused && ivars->retryTimer != nullptr) {
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        ivars->retryTimer->WakeAtTime(kIOTimerClockUptimeRaw,
                                      now + ATD_RETRY_DELAY_NS, 0);
    }
}

void
IMPL(AndroidTetherDriver, RetryTimer)
{
    SubmitReads();
}

/* ---------------------------------------------------------------- */

/*
 * Hand the frames in one completed transfer to the network stack.
 *
 * Each frame needs a packet from the receive submission queue; if the
 * stack has not given us one, the frame is dropped. That is the correct
 * behaviour -- the alternative is buffering indefinitely on behalf of a
 * stack that is already behind.
 */
void
AndroidTetherDriver::DeliverFrames(const uint8_t *buf, uint32_t length)
{
    rndis_frame_t frames[ATD_DEQUEUE_BATCH];
    uint32_t found = 0;

    const rndis_parse_result_t parsed =
        rndis_parse_transfer(buf, length, frames, ATD_DEQUEUE_BATCH,
                             RNDIS_ETHERNET_MAX_FRAME, &found);

    if (parsed != RNDIS_PARSE_OK) {
        /*
         * Not fatal, and not necessarily the device's fault: a transfer
         * holding more records than the batch size ends here too. The
         * frames found before the stop are still delivered below.
         */
        ATDLog("transfer of %u bytes: %s after %u frames", length,
               rndis_parse_result_name(parsed), found);
    }

    if (found == 0)
        return;

    IOUserNetworkPacket *packets[ATD_DEQUEUE_BATCH];
    const uint32_t available = ivars->rxSubmission->DequeuePackets(packets, found);

    for (uint32_t i = 0; i < available; i++) {
        IOUserNetworkPacket *packet = packets[i];

        uint8_t *base = (uint8_t *)packet->getDataVirtualAddress();
        const uint16_t offset = packet->getDataOffset();

        if (base == nullptr) {
            ivars->pool->DeallocatePacket(packet);
            ivars->rxDropped++;
            continue;
        }

        memcpy(base + offset, buf + frames[i].offset, frames[i].length);

        bool ok = true;
        ok &= (packet->setDataOffset(offset) == kIOReturnSuccess);
        ok &= (packet->setDataLength(frames[i].length) == kIOReturnSuccess);

        /*
         * The frame starts with a 14-byte Ethernet header. Apple's sample
         * passes 0 here, but its packet is a synthetic IP datagram with
         * no link header at all; ours is a real Ethernet frame straight
         * off the wire, so the length is the real one.
         */
        ok &= (packet->SetLinkHeaderLength(RNDIS_ETHERNET_HEADER_LEN) ==
               kIOReturnSuccess);

        if (!ok || ivars->rxCompletion->EnqueuePacket(packet) != kIOReturnSuccess) {
            ivars->pool->DeallocatePacket(packet);
            ivars->rxDropped++;
            continue;
        }

        ivars->rxFrames++;
    }

    /* Frames we parsed but had no packet for. Worth counting: it is the
     * difference between "the phone is quiet" and "we cannot keep up". */
    ivars->rxDropped += (found - available);
}

void
IMPL(AndroidTetherDriver, ReadComplete)
{
    const uint64_t slot = *(uint64_t *)action->GetReference();

    if (slot >= ATD_RX_SLOTS)
        return;

    ivars->rxSlotBusy[slot] = false;

    if (ivars->stopping || !ivars->interfaceEnabled)
        return;

    if (status == kIOReturnAborted)
        return;     /* the interface is going down; SubmitReads would fight it */

    if (status == kIOUSBPipeStalled) {
        ATDLog("the receive pipe stalled; clearing");
        ivars->inPipe->ClearStall(true);
    } else if (status != kIOReturnSuccess) {
        ATDLog("a read failed: 0x%08x", status);
    } else if (actualByteCount > 0) {
        DeliverFrames(ivars->rxBytes[slot], actualByteCount);
    }

    SubmitReads();
}

/* ---------------------------------------------------------------- */

/*
 * Copy one Ethernet frame into a transmit slot behind its RNDIS header
 * and start the transfer.
 *
 * The frame is copied rather than sent in place because RNDIS puts a
 * 44-byte header in front of it and the packet we are handed has no room
 * there. Copying also means the network stack's packet can be returned
 * immediately, instead of being held until the USB write completes.
 */
kern_return_t
AndroidTetherDriver::SendFrame(const uint8_t *frame, uint32_t length)
{
    if (ivars->stopping || !ivars->interfaceEnabled || ivars->outPipe == nullptr)
        return kIOReturnNotReady;

    uint32_t slot = ATD_TX_SLOTS;
    for (uint32_t i = 0; i < ATD_TX_SLOTS; i++) {
        if (!ivars->txSlotBusy[i]) {
            slot = i;
            break;
        }
    }

    if (slot == ATD_TX_SLOTS)
        return kIOReturnNoResources;    /* every transfer is still in flight */

    uint8_t *dst = ivars->txBytes[slot];
    const size_t header = rndis_build_packet_header(dst, ivars->txBufferSize, length);
    if (header == 0)
        return kIOReturnBadArgument;

    memcpy(dst + header, frame, length);

    const kern_return_t ret = ivars->outPipe->AsyncIO(ivars->txBuffer[slot],
                                                      (uint32_t)(header + length),
                                                      ivars->txAction[slot],
                                                      ATD_BULK_OUT_TIMEOUT_MS);
    if (ret != kIOReturnSuccess)
        return ret;

    ivars->txSlotBusy[slot] = true;
    return kIOReturnSuccess;
}

void
IMPL(AndroidTetherDriver, TxPacketAvailable)
{
    if (ivars->stopping || !ivars->interfaceEnabled)
        return;

    for (;;) {
        IOUserNetworkPacket *packets[ATD_DEQUEUE_BATCH];
        const uint32_t count =
            ivars->txSubmission->DequeuePackets(packets, ATD_DEQUEUE_BATCH);

        if (count == 0)
            break;

        for (uint32_t i = 0; i < count; i++) {
            IOUserNetworkPacket *packet = packets[i];

            const uint8_t *base = (const uint8_t *)packet->getDataVirtualAddress();
            const uint16_t offset = packet->getDataOffset();
            const uint32_t length = packet->getDataLength();

            if (base != nullptr && length > 0 &&
                length <= RNDIS_ETHERNET_MAX_FRAME &&
                SendFrame(base + offset, length) == kIOReturnSuccess) {
                ivars->txFrames++;
            } else {
                ivars->txDropped++;
            }

            /*
             * The packet goes back whether or not the frame went out: its
             * contents are already copied, and holding it would stall the
             * stack rather than retry anything.
             */
            if (ivars->txCompletion->EnqueuePacket(packet) != kIOReturnSuccess)
                ivars->pool->DeallocatePacket(packet);
        }

        if (count < ATD_DEQUEUE_BATCH)
            break;
    }
}

void
IMPL(AndroidTetherDriver, WriteComplete)
{
    const uint64_t slot = *(uint64_t *)action->GetReference();

    if (slot >= ATD_TX_SLOTS)
        return;

    ivars->txSlotBusy[slot] = false;

    if (status == kIOUSBPipeStalled) {
        ATDLog("the transmit pipe stalled; clearing");
        ivars->outPipe->ClearStall(true);
    } else if (status != kIOReturnSuccess && status != kIOReturnAborted) {
        ATDLog("a write failed: 0x%08x", status);
    }
}
