//
// Created by victor on 4/21/26.
//

#ifndef POSEIDON_QUASAR_ROUTE_H
#define POSEIDON_QUASAR_ROUTE_H

#include "quasar.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Magic number for route message wire format */
#define QUASAR_ROUTE_MAGIC   0x51524F54u
/** Version for route message wire format */
#define QUASAR_ROUTE_VERSION 1u

/**
 * Serializes a route message to a binary buffer for network transmission.
 * Caller must free the returned buffer.
 *
 * @param msg     Route message to serialize
 * @param buf     Output: pointer to allocated buffer
 * @param buf_len Output: length of allocated buffer
 * @return        0 on success, -1 on failure
 */
int quasar_route_message_serialize(const quasar_route_message_t* msg, uint8_t** buf, size_t* buf_len);

/**
 * Deserializes a binary buffer into a route message.
 * Caller must destroy the returned message.
 *
 * @param data    Raw bytes received from network
 * @param len     Length of data
 * @return        New route message, or NULL on failure
 */
quasar_route_message_t* quasar_route_message_deserialize(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_QUASAR_ROUTE_H
