/* Poseidon — QUIC client-side transport payload definitions
 *
 * The TYPE values themselves live in src/Actor/message.h (the global
 * message_type_e). This header owns the per-connection payload
 * structs, mirroring the per-subsystem message pattern established by
 * ClientAPIs/unix_connection_messages.h and
 * ClientAPIs/tcp_connection_messages.h.
 *
 * The destroyer helpers that free these payloads are file-static in
 * quic_connection.c: they are handed to actor_send as function
 * pointers and have no meaning outside that translation unit.
 *
 * The payloads carry plain heap buffers rather than buffer_t: msquic
 * frees the buffer of a completed send itself, and the buffers it
 * hands over for received data belong to msquic as well, so both
 * directions use plain malloc'd memory.
 */
#ifndef POSEIDON_QUIC_CONNECTION_MESSAGES_H
#define POSEIDON_QUIC_CONNECTION_MESSAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Inbound bytes received on the client's stream. The data is a copy:
 * the buffers msquic hands over belong to msquic and are only valid
 * for the duration of the callback. The connection actor reassembles
 * the frames from them.
 */
typedef struct poseidon_quic_connection_data_payload_t {
  uint8_t* data;
  size_t len;
} poseidon_quic_connection_data_payload_t;

/* Outbound frame submitted by the session actor. The data is the
 * caller-owned, already-framed payload: the 4-byte big-endian length
 * prefix has been added by the transport's send path, so the
 * connection writes the bytes out unchanged.
 */
typedef struct poseidon_quic_connection_write_payload_t {
  uint8_t* data;
  size_t len;
} poseidon_quic_connection_write_payload_t;

/* Close request submitted by the session actor or a timer. The
 * graceful flag mirrors the public poseidon_quic_connection_close()
 * parameter: true = shut the stream down gracefully; false = tear the
 * connection down immediately.
 */
typedef struct poseidon_quic_connection_close_payload_t {
  bool graceful;
} poseidon_quic_connection_close_payload_t;

#endif /* POSEIDON_QUIC_CONNECTION_MESSAGES_H */
