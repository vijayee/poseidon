/* Poseidon — TCP client-side transport payload definitions
 *
 * The TYPE values themselves live in src/Actor/message.h (the global
 * message_type_e). This header owns the per-connection payload
 * structs, mirroring the per-subsystem message pattern established by
 * ClientAPIs/unix_connection_messages.h and
 * Channel/channel_manager_messages.h. Keeping the layouts out of
 * message.h keeps the global enum header free of poll-dancer, platform
 * socket, and client-session dependencies.
 *
 * The destroyer helpers that free these payloads are file-static in
 * tcp_connection.c: they are handed to actor_send as function pointers
 * and have no meaning outside that translation unit.
 */
#ifndef POSEIDON_TCP_CONNECTION_MESSAGES_H
#define POSEIDON_TCP_CONNECTION_MESSAGES_H

#include <stdbool.h>
#include <stdint.h>

struct buffer_t;

/* Outbound frame submitted by the session actor or by the connection's
 * own request handler. The buffer is the caller-owned, already-framed
 * payload: the 4-byte big-endian length prefix has been added by the
 * transport's send path, so the connection writes the bytes out
 * unchanged.
 */
typedef struct poseidon_tcp_connection_write_payload_t {
  struct buffer_t* buffer;
} poseidon_tcp_connection_write_payload_t;

/* Close request submitted by the session actor or a timer. The
 * graceful flag mirrors the public poseidon_tcp_connection_close()
 * parameter: true = drain pending writes before shutting down the fd;
 * false = drop the pending write and tear the connection down
 * immediately.
 */
typedef struct poseidon_tcp_connection_close_payload_t {
  bool graceful;
} poseidon_tcp_connection_close_payload_t;

#endif /* POSEIDON_TCP_CONNECTION_MESSAGES_H */
