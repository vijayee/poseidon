//
// Created by victor on 4/20/26.
//

#ifndef POSEIDON_QUASAR_MESSAGE_ID_H
#define POSEIDON_QUASAR_MESSAGE_ID_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// MESSAGE ID TYPE
// ============================================================================

/**
 * Unique identifier for Quasar route messages.
 * Provides total ordering and serialization for network transmission.
 * Follows WaveDB's transaction_id_t pattern: CLOCK_MONOTONIC timestamp
 * with atomic counter for same-timestamp uniqueness.
 */
typedef struct quasar_message_id_t {
    uint64_t time;    /**< Seconds from CLOCK_MONOTONIC */
    uint64_t nanos;   /**< Nanoseconds within second */
    uint64_t count;   /**< Atomic sequence counter for same-timestamp uniqueness */
} quasar_message_id_t;

/** Size in bytes of a serialized message ID */
#define QUASAR_MESSAGE_ID_SIZE 24

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * Initializes the global message ID generator.
 * Must be called once before quasar_message_id_get_next().
 * Safe to call multiple times (uses pthread_once / InitOnceExecuteOnce).
 */
void quasar_message_id_init(void);

// ============================================================================
// GENERATION
// ============================================================================

/**
 * Generates a unique message ID.
 * Thread-safe, no lock needed. Uses CLOCK_MONOTONIC + atomic counter.
 *
 * @return  New unique message ID
 */
quasar_message_id_t quasar_message_id_get_next(void);

// ============================================================================
// COMPARISON
// ============================================================================

/**
 * Compares two message IDs lexicographically (time, nanos, count).
 *
 * @param a  First ID
 * @param b  Second ID
 * @return   1 if a > b, -1 if a < b, 0 if equal
 */
int quasar_message_id_compare(const quasar_message_id_t* a, const quasar_message_id_t* b);

// ============================================================================
// SERIALIZATION
// ============================================================================

/**
 * Serializes a message ID to 24 bytes in network byte order.
 *
 * @param id  Message ID to serialize
 * @param buf Output buffer (must have space for QUASAR_MESSAGE_ID_SIZE bytes)
 */
void quasar_message_id_serialize(const quasar_message_id_t* id, uint8_t* buf);

/**
 * Deserializes a message ID from 24 bytes in network byte order.
 *
 * @param id  Output: deserialized message ID
 * @param buf Input buffer (QUASAR_MESSAGE_ID_SIZE bytes)
 */
void quasar_message_id_deserialize(quasar_message_id_t* id, const uint8_t* buf);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_QUASAR_MESSAGE_ID_H