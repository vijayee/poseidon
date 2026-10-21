/* Poseidon — Unix client-side transport payload definitions
 *
 * The TYPE values themselves live in src/Actor/message.h (the global
 * message_type_e). This header owns the per-connection and per-listener
 * payload structs, mirroring the per-subsystem message pattern
 * established by Network/Meridian/meridian_conn_messages.h and
 * Channel/channel_manager_messages.h. Keeping the layouts out of
 * message.h keeps the global enum header free of poll-dancer, platform
 * socket, and client-session dependencies.
 *
 * The destroyer helpers that free these payloads are file-static in
 * unix_connection.c: they are handed to actor_send as function
 * pointers and have no meaning outside that translation unit.
 */
#ifndef POSEIDON_UNIX_CONNECTION_MESSAGES_H
#define POSEIDON_UNIX_CONNECTION_MESSAGES_H

#include <stdbool.h>
#include <stdint.h>

struct buffer_t;
struct platform_socket_t;
struct pd_watcher;

/* Inbound bytes read off the connection's socket. The buffer holds
 * the raw bytes as they arrived; the connection actor reassembles
 * frames from them. The destroyer frees both the buffer and the
 * wrapper.
 */
typedef struct poseidon_unix_connection_data_payload_t {
  struct buffer_t* buffer;
} poseidon_unix_connection_data_payload_t;

/* Outbound frame submitted by the session actor. The buffer is the
 * caller-owned, already-framed payload: the 4-byte big-endian length
 * prefix has been added by the transport's send path, so the
 * connection writes the bytes out unchanged.
 */
typedef struct poseidon_unix_connection_write_payload_t {
  struct buffer_t* buffer;
} poseidon_unix_connection_write_payload_t;

/* Close request submitted by the session actor or a timer. The
 * graceful flag mirrors the public poseidon_unix_connection_close()
 * parameter: true = drain pending writes before shutting down the fd;
 * false = drop the pending write and tear the connection down
 * immediately.
 */
typedef struct poseidon_unix_connection_close_payload_t {
  bool graceful;
} poseidon_unix_connection_close_payload_t;

/* Watcher update / stop request routed from the per-connection actor
 * to the listener actor. The watcher is the I/O watcher's pointer
 * (borrowed — the connection owns it until the stop is processed). The
 * events field is a uint32_t pd_event_t bitmask; it is carried as a
 * uint32_t (rather than as a typed pd_event_t) so this header does
 * not need to pull in poll-dancer.h just to name the enum. The sock
 * field is only populated on STOP_WATCHER messages that transfer the
 * socket's ownership to the listener; UPDATE_WATCHER leaves it NULL.
 */
typedef struct poseidon_unix_server_watcher_update_payload_t {
  struct pd_watcher* watcher;
  uint32_t events;
  struct platform_socket* sock;
} poseidon_unix_server_watcher_update_payload_t;

#endif /* POSEIDON_UNIX_CONNECTION_MESSAGES_H */
