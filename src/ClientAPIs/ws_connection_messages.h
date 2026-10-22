/* Poseidon — WebSocket client-side transport payload definitions
 *
 * The TYPE values themselves live in src/Actor/message.h (the global
 * message_type_e). This header owns the per-connection payload
 * structs, mirroring the per-subsystem message pattern established by
 * ClientAPIs/unix_connection_messages.h and
 * ClientAPIs/tcp_connection_messages.h. Keeping the layouts out of
 * message.h keeps the global enum header free of poll-dancer, platform
 * socket, and client-session dependencies.
 *
 * The destroyer helpers that free these payloads are file-static in
 * ws_connection.c: they are handed to actor_send as function pointers
 * and have no meaning outside that translation unit.
 */
#ifndef POSEIDON_WS_CONNECTION_MESSAGES_H
#define POSEIDON_WS_CONNECTION_MESSAGES_H

#include <stdbool.h>
#include <stdint.h>

struct buffer_t;

/* Outbound frame submitted by the session actor. The buffer holds the
 * bytes as they go on the wire: the 4-byte big-endian length prefix,
 * the CBOR payload and the WebSocket frame header around them. The
 * connection's write actor owns the buffer from here on.
 */
typedef struct poseidon_ws_connection_write_payload_t {
  struct buffer_t* buffer;
} poseidon_ws_connection_write_payload_t;

/* Close request submitted by the session actor or a timer. The
 * graceful flag mirrors the public poseidon_ws_connection_close()
 * parameter: true = drain pending writes before shutting down the fd;
 * false = drop the pending write and tear the connection down
 * immediately.
 */
typedef struct poseidon_ws_connection_close_payload_t {
  bool graceful;
} poseidon_ws_connection_close_payload_t;

#endif /* POSEIDON_WS_CONNECTION_MESSAGES_H */
