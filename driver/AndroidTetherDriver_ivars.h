/*
 * AndroidTetherDriver_ivars.h -- everything one attached phone needs.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * The ivars struct is separate from the .iig because two translation units
 * touch it: the lifecycle and control channel in AndroidTetherDriver.cpp,
 * and the packet path in AndroidTetherDriver_datapath.cpp.
 *
 * Concurrency: every field here is touched only from the driver's single
 * dispatch queue (`dispatchQueue`). The pipe completions, the transmit
 * doorbell and the retry timer are all delivered on it, which is what
 * makes the absence of locking correct rather than lucky. Anything added
 * later that runs elsewhere needs its own answer to that question.
 */

#ifndef ANDROIDTETHERDRIVER_IVARS_H
#define ANDROIDTETHERDRIVER_IVARS_H

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSAction.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>
#include <NetworkingDriverKit/IOUserNetworkRxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxCompletionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxCompletionQueue.h>

#include "rndis.h"

/*
 * How many bulk IN reads are in flight at once. One is enough to be
 * correct and not enough to keep the pipe busy: while a completion is
 * being handled the device has nowhere to put the next transfer, which on
 * a phone that is actually sending shows up as a throughput ceiling rather
 * than as an error.
 */
#define ATD_RX_SLOTS 4

/*
 * Outgoing transfers in flight. Each one owns its buffer, because the
 * frame has to be copied out of the network stack's packet anyway: RNDIS
 * puts a header in front of the Ethernet frame, and the packet buffer we
 * are handed has no room in front of the data for it.
 */
#define ATD_TX_SLOTS 8

/* Packets held by the pool, and the size of each buffer in it. 2048 is
 * the smallest power of two that holds a full Ethernet frame plus the
 * headroom the stack may ask for in front of it. */
#define ATD_POOL_PACKETS     256
#define ATD_POOL_BUFFERS     256
#define ATD_POOL_BUFFER_SIZE 2048

/* Depth of each of the four packet queues. */
#define ATD_QUEUE_CAPACITY 64

/* Dequeue at most this many packets from a submission queue per callback. */
#define ATD_DEQUEUE_BATCH 16

struct AndroidTetherDriver_IVars {
    /* --- USB --- */
    IOUSBHostInterface      *controlInterface;  /* the provider; retained by IOKit */
    IOUSBHostInterface      *dataInterface;     /* found on the same device */
    IOUSBHostDevice         *device;
    IOUSBHostPipe           *inPipe;
    IOUSBHostPipe           *outPipe;
    uint8_t                  controlInterfaceNumber;
    bool                     controlInterfaceOpen;
    bool                     dataInterfaceOpen;
    bool                     deviceOpen;

    /* --- dispatch --- */
    IODispatchQueue         *dispatchQueue;
    IOTimerDispatchSource   *retryTimer;
    OSAction                *retryTimerAction;
    OSAction                *txDoorbellAction;

    /* --- control channel --- */
    IOBufferMemoryDescriptor *controlBuffer;
    uint8_t                  *controlBytes;     /* mapped address of the above */
    uint32_t                  nextRequestId;

    /* What the device told us in REMOTE_NDIS_INITIALIZE_CMPLT. */
    uint32_t                  deviceMaxTransferSize;

    /* --- receive --- */
    IOBufferMemoryDescriptor *rxBuffer[ATD_RX_SLOTS];
    OSAction                 *rxAction[ATD_RX_SLOTS];
    uint8_t                  *rxBytes[ATD_RX_SLOTS];
    uint32_t                  rxBufferSize;
    bool                      rxSlotBusy[ATD_RX_SLOTS];

    /* --- transmit --- */
    IOBufferMemoryDescriptor *txBuffer[ATD_TX_SLOTS];
    OSAction                 *txAction[ATD_TX_SLOTS];
    uint8_t                  *txBytes[ATD_TX_SLOTS];
    uint32_t                  txBufferSize;
    bool                      txSlotBusy[ATD_TX_SLOTS];

    /* --- networking --- */
    IOUserNetworkPacketBufferPool  *pool;
    IOUserNetworkRxSubmissionQueue *rxSubmission;
    IOUserNetworkRxCompletionQueue *rxCompletion;
    IOUserNetworkTxSubmissionQueue *txSubmission;
    IOUserNetworkTxCompletionQueue *txCompletion;

    /* --- state --- */
    bool                      interfaceEnabled;
    bool                      stopping;

    /* Counters worth having when a user reports "it is slow" rather than
     * "it does not work". Logged on stop, not exported. */
    uint64_t                  rxFrames;
    uint64_t                  rxDropped;
    uint64_t                  txFrames;
    uint64_t                  txDropped;
};

#endif /* ANDROIDTETHERDRIVER_IVARS_H */
