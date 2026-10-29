/*
 * rndis_control.h -- building and checking RNDIS control messages.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * [MS-RNDIS] 2.2.2 through 2.2.13.
 *
 * The control channel is the part of the handshake that decides whether an
 * interface ever appears, and it is all arithmetic over a byte buffer: what
 * to send, and whether what came back is a plausible answer to it. None of
 * that needs USB, so none of it lives in the driver. The driver moves the
 * bytes; these functions decide what the bytes are and whether to believe
 * the reply.
 *
 * Every builder returns the number of bytes to send, or 0 if the request
 * will not fit the buffer. Every checker is handed exactly what the device
 * returned, including its length, and is responsible for trusting nothing
 * in it.
 */

#ifndef RNDIS_CONTROL_H
#define RNDIS_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#include "rndis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why a reply was rejected. */
typedef enum {
    RNDIS_REPLY_OK = 0,
    RNDIS_REPLY_TOO_SHORT,      /* fewer bytes than the message needs */
    RNDIS_REPLY_WRONG_TYPE,     /* not the completion we asked for */
    RNDIS_REPLY_WRONG_ID,       /* a completion, but to a different request */
    RNDIS_REPLY_BAD_MSG_LEN,    /* msg_len disagrees with what arrived */
    RNDIS_REPLY_DEVICE_ERROR,   /* well formed, and the device said no */
    RNDIS_REPLY_BAD_PAYLOAD     /* info buffer not inside the message */
} rndis_reply_result_t;

/* What a device told us in REMOTE_NDIS_INITIALIZE_CMPLT and we act on. */
typedef struct {
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t max_transfer_size;         /* most the device will send at once */
    uint32_t max_packets_per_transfer;
    uint32_t packet_alignment_factor;   /* as a power of two */
} rndis_init_reply_t;

/* ---- builders ---- */

size_t rndis_build_initialize(uint8_t *dst, size_t dst_len,
                              uint32_t request_id, uint32_t max_transfer_size);

size_t rndis_build_query(uint8_t *dst, size_t dst_len,
                         uint32_t request_id, uint32_t oid);

/* A SET whose info buffer is a single 32-bit value, which covers every SET
 * this driver sends. */
size_t rndis_build_set_u32(uint8_t *dst, size_t dst_len,
                           uint32_t request_id, uint32_t oid, uint32_t value);

size_t rndis_build_halt(uint8_t *dst, size_t dst_len, uint32_t request_id);

/* ---- reply checking ---- */

/*
 * Common checks for any completion: long enough, the completion of
 * sent_type, carrying request_id, internally consistent, and successful.
 * Pass request_id 0 to skip the id check, which HALT and RESET need since
 * they carry none.
 */
rndis_reply_result_t rndis_check_completion(const uint8_t *buf, uint32_t len,
                                            uint32_t sent_type,
                                            uint32_t request_id);

/* REMOTE_NDIS_INITIALIZE_CMPLT, with the fields worth keeping. */
rndis_reply_result_t rndis_parse_initialize_reply(const uint8_t *buf, uint32_t len,
                                                  uint32_t request_id,
                                                  rndis_init_reply_t *out);

/*
 * REMOTE_NDIS_QUERY_CMPLT. On success *info and *info_len point into buf at
 * the returned information buffer, which is validated to lie inside the
 * message that actually arrived.
 */
rndis_reply_result_t rndis_parse_query_reply(const uint8_t *buf, uint32_t len,
                                             uint32_t request_id,
                                             const uint8_t **info,
                                             uint32_t *info_len);

const char *rndis_reply_result_name(rndis_reply_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* RNDIS_CONTROL_H */
