/* Poseidon — iOS XPC transport backend
 *
 * The XPC backend is a placeholder: XPC connections are managed by
 * launchd and the listener callback dispatches to on_message. What
 * lives here is the actor-shaped transport body every backend shares,
 * so the platform bridge has the same starting point as the other
 * backends once it lands.
 *
 * There is no connection path yet: the bridge has no accept callback
 * and no per-connection actor, so the connection table stays empty
 * until the platform work arrives. The shape to follow then is the one
 * of the Unix and TCP backends.
 *
 * The transport object is the actor-shaped body declared in
 * transport.h; the backend's private state lives in a backend struct
 * whose first member is that body.
 */

#ifndef POSEIDON_TRANSPORT_XPC_H
#define POSEIDON_TRANSPORT_XPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "transport.h"


#ifdef __cplusplus
extern "C" {
#endif

#ifdef __APPLE__
/**
 * Creates an XPC transport.
 *
 * @param service_name  Name of the XPC service
 * @param manager       Shared channel manager
 * @return              New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_xpc_create(
    const char* service_name,
    poseidon_channel_manager_t* manager);

/**
 * Releases every resource the transport owns: the connection table,
 * the I/O loop and the transport actor. The transport pointer is
 * invalid after this call.
 */
void poseidon_transport_xpc_destroy(poseidon_transport_t* transport);

/**
 * Starts the transport. The XPC service itself is managed by launchd.
 *
 * @return 0 on success, -1 on failure
 */
int poseidon_transport_xpc_start(poseidon_transport_t* transport);

/**
 * Stops the transport. Safe to call on a transport that was never
 * started.
 */
int poseidon_transport_xpc_stop(poseidon_transport_t* transport);

/**
 * Sends a payload to the client identified by client_id. The reply
 * dictionary is written by the XPC bridge.
 *
 * @return 0 on success, -1 if the client is unknown
 */
int poseidon_transport_xpc_send(poseidon_transport_t* transport,
                                int client_id,
                                const uint8_t* data,
                                size_t len);
#endif /* __APPLE__ */

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_TRANSPORT_XPC_H */
