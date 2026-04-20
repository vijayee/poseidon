#ifndef POSEIDON_MERIDIAN_RELAY_SERVER_H
#define POSEIDON_MERIDIAN_RELAY_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

#include "../../RefCounter/refcounter.h"
#include "../../Util/threadding.h"
#include "msquic.h"

/**
 * @brief Maximum number of concurrent clients supported by relay server
 */
#define MERIDIAN_RELAY_MAX_CLIENTS 256

/**
 * @brief Sentinel value indicating no endpoint ID has been assigned
 */
#define MERIDIAN_RELAY_ENDPOINT_ID_NONE 0

/**
 * @brief Per-connected-client state in the relay server
 */
typedef struct meridian_relay_client_t {
    uint32_t endpoint_id;               /**< Assigned endpoint ID (0 = unassigned) */
    HQUIC connection;                    /**< QUIC connection handle */
    struct sockaddr_storage remote_addr; /**< Observed remote address */
    uint64_t last_activity_ms;           /**< Last activity timestamp */
    bool authenticated;                  /**< Has completed initial handshake */
} meridian_relay_client_t;

/**
 * @brief Main relay server state
 */
typedef struct meridian_relay_server_t {
    refcounter_t refcounter;                              /**< Reference counter for lifetime management */
    const struct QUIC_API_TABLE* msquic;                 /**< MsQuic API function table */
    HQUIC registration;                                    /**< MsQuic registration handle */
    HQUIC configuration;                                   /**< MsQuic listener configuration */
    HQUIC listener;                                        /**< MsQuic listener handle */
    uint16_t listen_port;                                 /**< Port the server is listening on */
    meridian_relay_client_t clients[MERIDIAN_RELAY_MAX_CLIENTS]; /**< Connected clients array */
    size_t num_clients;                                   /**< Current number of connected clients */
    uint32_t next_endpoint_id;                            /**< Next endpoint ID to assign */
    PLATFORMLOCKTYPE(lock);                               /**< Thread safety lock for client list */
} meridian_relay_server_t;

/**
 * @brief Configuration for relay server creation
 */
typedef struct meridian_relay_server_config_t {
    const char* alpn;                  /**< Application-Layer Protocol Negotiation identifier */
    uint16_t listen_port;              /**< Port to listen on */
    uint32_t idle_timeout_ms;          /**< Connection idle timeout in milliseconds */
    uint32_t keepalive_interval_ms;    /**< Keep-alive probe interval in milliseconds */
    uint32_t max_datagram_size;        /**< Maximum datagram payload size */
} meridian_relay_server_config_t;

/**
 * @brief Runtime statistics for relay server monitoring
 */
typedef struct meridian_relay_server_stats_t {
    size_t num_clients;        /**< Current number of connected clients */
    uint64_t datagrams_forwarded; /**< Total datagrams successfully forwarded */
    uint64_t datagrams_dropped;   /**< Total datagrams dropped due to errors or no route */
    uint64_t addr_requests_served; /**< Total address discovery requests served */
} meridian_relay_server_stats_t;

/**
 * @brief Create a new relay server instance
 *
 * @param[in] msquic Pointer to MsQuic API function table
 * @param[in] config Server configuration parameters
 * @return Newly created server or NULL on failure
 */
meridian_relay_server_t*
meridian_relay_server_create(
    const struct QUIC_API_TABLE* msquic,
    const meridian_relay_server_config_t* config
);

/**
 * @brief Destroy a relay server instance
 *
 * Releases all resources associated with the server. Server must be stopped
 * before destruction.
 *
 * @param[in] server Server to destroy
 */
void
meridian_relay_server_destroy(
    meridian_relay_server_t* server
);

/**
 * @brief Start the relay server listening for connections
 *
 * @param[in] server Server to start
 * @return 0 on success, error code on failure
 */
int
meridian_relay_server_start(
    meridian_relay_server_t* server
);

/**
 * @brief Stop the relay server and disconnect all clients
 *
 * @param[in] server Server to stop
 */
int
meridian_relay_server_stop(
    meridian_relay_server_t* server
);

/**
 * @brief Get current runtime statistics
 *
 * @param[in] server Server to query
 * @param[out] stats Output statistics structure
 * @return 0 on success, -1 on failure
 */
int
meridian_relay_server_get_stats(
    meridian_relay_server_t* server,
    meridian_relay_server_stats_t* stats
);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_MERIDIAN_RELAY_SERVER_H */
