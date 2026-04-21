//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_PACKET_H
#define POSEIDON_MERIDIAN_PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "meridian.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
#include <cbor.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// PACKET TYPE CONSTANTS
// ============================================================================

/**
 * Meridian protocol packet types.
 * These are the wire format identifiers for all protocol messages.
 */

/** Gossip: Periodic node exchange with peers */
#define MERIDIAN_PACKET_TYPE_GOSSIP           10
/** Gossip Pull: Request gossip from peer */
#define MERIDIAN_PACKET_TYPE_GOSSIP_PULL      23
/** Push: Push nodes to peer */
#define MERIDIAN_PACKET_TYPE_PUSH             11
/** Pull: Pull nodes from peer */
#define MERIDIAN_PACKET_TYPE_PULL             12
/** Ping: Latency probe request */
#define MERIDIAN_PACKET_TYPE_PING             14
/** Pong: Latency probe response */
#define MERIDIAN_PACKET_TYPE_PONG             15
/** Create Rendezvous: Request rendezvous point creation */
#define MERIDIAN_PACKET_TYPE_CREATE_RENDV     16
/** Return Rendezvous: Rendezvous point info response */
#define MERIDIAN_PACKET_TYPE_RET_RENDV        17
/** Return Error: Error response */
#define MERIDIAN_PACKET_TYPE_RET_ERROR        18
/** Return Info: Info response */
#define MERIDIAN_PACKET_TYPE_RET_INFO         19
/** Return Ping: Ping response */
#define MERIDIAN_PACKET_TYPE_RET_PING         13
/** Return Response: General response with closest node + targets */
#define MERIDIAN_PACKET_TYPE_RET_RESPONSE     9
/** Info: Info message */
#define MERIDIAN_PACKET_TYPE_INFO             22

/** Request: Measure latency via TCP */
#define MERIDIAN_PACKET_TYPE_REQ_MEASURE_TCP   3
/** Request: Measure latency via ICMP ping */
#define MERIDIAN_PACKET_TYPE_REQ_MEASURE_PING  5
/** Request: Find closest node via TCP */
#define MERIDIAN_PACKET_TYPE_REQ_CLOSEST_TCP   2
/** Request: Find closest node via ICMP ping */
#define MERIDIAN_PACKET_TYPE_REQ_CLOSEST_PING  4
/** Request: Find closest node via DNS */
#define MERIDIAN_PACKET_TYPE_REQ_CLOSEST_DNS   6
/** Request: Measure latency via DNS */
#define MERIDIAN_PACKET_TYPE_REQ_MEASURE_DNS   7
/** Request: Find node meeting constraint via TCP */
#define MERIDIAN_PACKET_TYPE_REQ_CONSTRAINT_TCP 1
/** Request: Find node meeting constraint via DNS */
#define MERIDIAN_PACKET_TYPE_REQ_CONSTRAINT_DNS 20
/** Request: Find node meeting constraint via ICMP ping */
#define MERIDIAN_PACKET_TYPE_REQ_CONSTRAINT_PING 21

// ============================================================================
// RELAY / NAT TRAVERSAL PACKET TYPES
// ============================================================================

/** Address request: Ask relay for observed public address */
#define MERIDIAN_PACKET_TYPE_ADDR_REQUEST    30
/** Address response: Relay reports observed address and assigned endpoint ID */
#define MERIDIAN_PACKET_TYPE_ADDR_RESPONSE   31
/** Relay datagram: Forward data between peers via relay */
#define MERIDIAN_PACKET_TYPE_RELAY_DATAGRAM  32
/** Punch request: Coordinate hole-punching through relay */
#define MERIDIAN_PACKET_TYPE_PUNCH_REQUEST   33
/** Punch sync: Synchronize hole-punching between peers */
#define MERIDIAN_PACKET_TYPE_PUNCH_SYNC      34
/** Relay ping: Keepalive probe */
#define MERIDIAN_PACKET_TYPE_RELAY_PING      35
/** Relay pong: Keepalive response */
#define MERIDIAN_PACKET_TYPE_RELAY_PONG      36
/** Endpoint gone: Notification that a peer disconnected */
#define MERIDIAN_PACKET_TYPE_ENDPOINT_GONE   37

/** Channel bootstrap: Join request for a specific channel topic */
#define MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP       40
/** Channel bootstrap reply: Response with connection info */
#define MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY  41

// ============================================================================
// PROTOCOL CONSTANTS
// ============================================================================

/** Maximum number of nodes in a single packet */
#define MERIDIAN_MAX_NODES 256

/**
 * Magic number for packet validation.
 * All packets must start with this value to be considered valid.
 */
#define MERIDIAN_MAGIC_NUMBER 0x0A0B0C0D

// ============================================================================
// PACKET STRUCTURES
// ============================================================================

/**
 * Base packet header used by all Meridian messages.
 * Contains the minimum fields required for routing and validation.
 *
 * Wire format (CBOR array):
 * [type, query_id_hi, query_id_lo, magic, rendv_addr, rendv_port]
 */
typedef struct meridian_packet_t {
    uint8_t type;           /**< Packet type (see MERIDIAN_PACKET_TYPE_* constants) */
    uint64_t query_id;     /**< Unique query identifier (split into hi/lo for CBOR) */
    uint32_t magic;        /**< Magic number for validation (must be MERIDIAN_MAGIC_NUMBER) */
    uint32_t rendv_addr;   /**< Rendezvous point address for NAT traversal */
    uint16_t rendv_port;   /**< Rendezvous point port for NAT traversal */
} meridian_packet_t;

/**
 * Gossip packet for node exchange.
 * Contains a list of target nodes to share with peers.
 *
 * Wire format (CBOR array):
 * [type, query_id_hi, query_id_lo, magic, rendv_addr, rendv_port, num_targets, ...targets]
 * Where each target is: [addr, port, rendv_addr, rendv_port]
 */
typedef struct meridian_gossip_packet_t {
    meridian_packet_t base;              /**< Base packet header */
    vec_t(meridian_node_t*) targets;     /**< Vector of target nodes to share */
} meridian_gossip_packet_t;

/**
 * Ping packet for latency measurement.
 * Contains nodes and their measured latencies.
 *
 * Wire format (CBOR array):
 * [type, query_id_hi, query_id_lo, magic, rendv_addr, rendv_port, num_nodes, ...nodes]
 * Where each node is: [addr, port, latency_us]
 */
typedef struct meridian_ping_packet_t {
    meridian_packet_t base;              /**< Base packet header */
    vec_t(meridian_node_t*) nodes;       /**< Vector of nodes to ping */
    vec_t(uint32_t) latencies;            /**< Measured latencies corresponding to nodes */
} meridian_ping_packet_t;

/**
 * A node and its latency measurement.
 * Used in response packets to report multiple measurements.
 */
typedef struct meridian_node_latency_t {
    uint32_t addr;       /**< Node IPv4 address */
    uint16_t port;       /**< Node port */
    uint32_t latency_us; /**< Latency in microseconds */
} meridian_node_latency_t;

/**
 * Response packet containing closest node and target list.
 * Used to respond to closest-node and measure requests.
 *
 * Wire format (CBOR array):
 * [type, query_id_hi, query_id_lo, magic, rendv_addr, rendv_port, closest_addr, closest_port, num_targets, ...targets]
 * Where each target is: [addr, port, latency_us]
 */
typedef struct meridian_ret_response_t {
    uint8_t type;                   /**< Packet type (MERIDIAN_PACKET_TYPE_RET_RESPONSE) */
    uint64_t query_id;              /**< Query ID this is responding to */
    uint32_t magic;                 /**< Magic number */
    uint32_t rendv_addr;            /**< Rendezvous point address for NAT traversal */
    uint16_t rendv_port;            /**< Rendezvous point port for NAT traversal */
    uint32_t closest_addr;          /**< Address of closest node found */
    uint16_t closest_port;          /**< Port of closest node */
    vec_t(meridian_node_latency_t) targets; /**< List of nodes with latencies */
} meridian_ret_response_t;

// ============================================================================
// RELAY / NAT TRAVERSAL STRUCTURES
// ============================================================================

/**
 * Address response from relay server.
 * Contains the relay's observation of the peer's public address
 * and the assigned endpoint ID for relay routing.
 *
 * Wire format (CBOR array):
 * [type, query_id_hi, query_id_lo, reflexive_addr, reflexive_port, endpoint_id]
 */
typedef struct meridian_addr_response_t {
    uint8_t type;               /**< Packet type (MERIDIAN_PACKET_TYPE_ADDR_RESPONSE) */
    uint64_t query_id;         /**< Query ID from the ADDR_REQUEST */
    uint32_t reflexive_addr;   /**< Observed public IPv4 address */
    uint16_t reflexive_port;   /**< Observed public port */
    uint32_t endpoint_id;      /**< Assigned relay endpoint ID */
} meridian_addr_response_t;

/**
 * Punch request for coordinating hole-punching through a relay.
 * Sent by peer A to the relay, which forwards it to peer B.
 *
 * Wire format (CBOR array):
 * [type, query_id_hi, query_id_lo, from_endpoint_id, target_addr, target_port]
 */
typedef struct meridian_punch_request_t {
    uint8_t type;               /**< Packet type (MERIDIAN_PACKET_TYPE_PUNCH_REQUEST) */
    uint64_t query_id;         /**< Query ID */
    uint32_t from_endpoint_id; /**< Sender's relay endpoint ID */
    uint32_t target_addr;      /**< Target peer's address */
    uint16_t target_port;      /**< Target peer's port */
} meridian_punch_request_t;

/**
 * Punch sync for hole-punching coordination.
 * Sent directly between peers to open NAT mappings.
 *
 * Wire format (CBOR array):
 * [type, query_id_hi, query_id_lo, from_endpoint_id, from_addr, from_port]
 */
typedef struct meridian_punch_sync_t {
    uint8_t type;               /**< Packet type (MERIDIAN_PACKET_TYPE_PUNCH_SYNC) */
    uint64_t query_id;         /**< Query ID */
    uint32_t from_endpoint_id; /**< Sender's relay endpoint ID */
    uint32_t from_addr;        /**< Sender's public address */
    uint16_t from_port;        /**< Sender's public port */
} meridian_punch_sync_t;

// ============================================================================
// GOSSIP PACKET OPERATIONS
// ============================================================================

/**
 * Creates an empty gossip packet with initialized targets vector.
 *
 * @return  New gossip packet, or NULL on allocation failure
 */
meridian_gossip_packet_t* meridian_gossip_packet_create(void);

/**
 * Destroys a gossip packet and its targets vector.
 *
 * @param pkt  Packet to destroy
 */
void meridian_gossip_packet_destroy(meridian_gossip_packet_t* pkt);

/**
 * Adds a target node to the gossip packet.
 *
 * @param pkt   Packet to add to
 * @param node  Target node to add
 * @return      0 on success, -1 on failure
 */
int meridian_gossip_packet_add_target(meridian_gossip_packet_t* pkt,
                                       meridian_node_t* node);

/**
 * Encodes a gossip packet into CBOR format for wire transmission.
 *
 * CBOR structure:
 * [type, query_id_hi, query_id_lo, magic, rendv_addr, rendv_port, num_targets, ...targets]
 * Where each target is: [addr, port, rendv_addr, rendv_port]
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item (caller must cbor_decref), or NULL on failure
 */
cbor_item_t* meridian_gossip_encode(const meridian_gossip_packet_t* pkt);

/**
 * Decodes a CBOR array into a gossip packet.
 * Validates magic number and target array structure.
 *
 * @param item  CBOR array to decode
 * @return      New gossip packet, or NULL on failure (invalid format, bad magic)
 */
meridian_gossip_packet_t* meridian_gossip_decode(cbor_item_t* item);

// ============================================================================
// PING PACKET OPERATIONS
// ============================================================================

/**
 * Creates an empty ping packet with initialized nodes and latencies vectors.
 *
 * @return  New ping packet, or NULL on allocation failure
 */
meridian_ping_packet_t* meridian_ping_packet_create(void);

/**
 * Destroys a ping packet and its vectors.
 *
 * @param pkt  Packet to destroy
 */
void meridian_ping_packet_destroy(meridian_ping_packet_t* pkt);

/**
 * Adds a node and its latency to the ping packet.
 *
 * @param pkt      Packet to add to
 * @param node     Node to add
 * @param latency  Measured latency in microseconds
 * @return         0 on success, -1 on failure
 */
int meridian_ping_packet_add_node(meridian_ping_packet_t* pkt,
                                   meridian_node_t* node, uint32_t latency);

/**
 * Encodes a ping packet into CBOR format.
 *
 * CBOR structure:
 * [type, query_id_hi, query_id_lo, magic, rendv_addr, rendv_port, num_nodes, ...nodes]
 * Where each node is: [addr, port, latency]
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_ping_encode(const meridian_ping_packet_t* pkt);

/**
 * Decodes a CBOR ping packet.
 *
 * @param item  CBOR array to decode
 * @return      New ping packet, or NULL on failure
 */
meridian_ping_packet_t* meridian_ping_decode(cbor_item_t* item);

// ============================================================================
// BASE PACKET OPERATIONS
// ============================================================================

/**
 * Encodes a base packet header into CBOR.
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_packet_encode(const meridian_packet_t* pkt);

/**
 * Decodes a CBOR array into an existing packet struct.
 *
 * @param item  CBOR array to decode
 * @param pkt   Output structure to fill
 * @return      0 on success, -1 on failure
 */
int meridian_packet_decode(cbor_item_t* item, meridian_packet_t* pkt);

// ============================================================================
// RESPONSE PACKET OPERATIONS
// ============================================================================

/**
 * Creates an empty response packet with initialized targets vector.
 *
 * @return  New response packet, or NULL on failure
 */
meridian_ret_response_t* meridian_ret_response_create(void);

/**
 * Destroys a response packet and its targets.
 *
 * @param pkt  Packet to destroy
 */
void meridian_ret_response_destroy(meridian_ret_response_t* pkt);

/**
 * Adds a target with latency to the response packet.
 *
 * @param pkt      Packet to add to
 * @param addr     Node address
 * @param port     Node port
 * @param latency  Latency in microseconds
 * @return         0 on success, -1 on failure
 */
int meridian_ret_response_add_target(meridian_ret_response_t* pkt,
                                      uint32_t addr, uint16_t port, uint32_t latency);

/**
 * Encodes a response packet into CBOR.
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_ret_response_encode(const meridian_ret_response_t* pkt);

/**
 * Decodes a CBOR response packet.
 *
 * @param item  CBOR array to decode
 * @return      New response packet, or NULL on failure
 */
meridian_ret_response_t* meridian_ret_response_decode(cbor_item_t* item);

// ============================================================================
// RELAY PACKET OPERATIONS
// ============================================================================

/**
 * Creates an address response packet.
 *
 * @param query_id       Query ID from the ADDR_REQUEST
 * @param reflexive_addr Observed public IPv4 address
 * @param reflexive_port Observed public port
 * @param endpoint_id    Assigned relay endpoint ID
 * @return               New address response, or NULL on failure
 */
meridian_addr_response_t* meridian_addr_response_create(uint64_t query_id,
                                                         uint32_t reflexive_addr,
                                                         uint16_t reflexive_port,
                                                         uint32_t endpoint_id);

/**
 * Destroys an address response packet.
 *
 * @param pkt  Packet to destroy
 */
void meridian_addr_response_destroy(meridian_addr_response_t* pkt);

/**
 * Encodes an address response into CBOR.
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_addr_response_encode(const meridian_addr_response_t* pkt);

/**
 * Decodes a CBOR address response.
 *
 * @param item  CBOR array to decode
 * @return      New address response, or NULL on failure
 */
meridian_addr_response_t* meridian_addr_response_decode(cbor_item_t* item);

/**
 * Creates a punch request packet.
 *
 * @param query_id        Query ID
 * @param from_endpoint_id Sender's relay endpoint ID
 * @param target_addr     Target peer's address
 * @param target_port     Target peer's port
 * @return                New punch request, or NULL on failure
 */
meridian_punch_request_t* meridian_punch_request_create(uint64_t query_id,
                                                         uint32_t from_endpoint_id,
                                                         uint32_t target_addr,
                                                         uint16_t target_port);

/**
 * Destroys a punch request packet.
 *
 * @param pkt  Packet to destroy
 */
void meridian_punch_request_destroy(meridian_punch_request_t* pkt);

/**
 * Encodes a punch request into CBOR.
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_punch_request_encode(const meridian_punch_request_t* pkt);

/**
 * Decodes a CBOR punch request.
 *
 * @param item  CBOR array to decode
 * @return      New punch request, or NULL on failure
 */
meridian_punch_request_t* meridian_punch_request_decode(cbor_item_t* item);

/**
 * Creates a punch sync packet.
 *
 * @param query_id        Query ID
 * @param from_endpoint_id Sender's relay endpoint ID
 * @param from_addr       Sender's public address
 * @param from_port       Sender's public port
 * @return                New punch sync, or NULL on failure
 */
meridian_punch_sync_t* meridian_punch_sync_create(uint64_t query_id,
                                                    uint32_t from_endpoint_id,
                                                    uint32_t from_addr,
                                                    uint16_t from_port);

/**
 * Destroys a punch sync packet.
 *
 * @param pkt  Packet to destroy
 */
void meridian_punch_sync_destroy(meridian_punch_sync_t* pkt);

/**
 * Encodes a punch sync into CBOR.
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_punch_sync_encode(const meridian_punch_sync_t* pkt);

/**
 * Decodes a CBOR punch sync.
 *
 * @param item  CBOR array to decode
 * @return      New punch sync, or NULL on failure
 */
meridian_punch_sync_t* meridian_punch_sync_decode(cbor_item_t* item);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * Creates a node from a latency entry.
 *
 * @param nl  Latency entry with addr/port
 * @return    New node, or NULL on failure
 */
meridian_node_t* meridian_node_from_latency(const meridian_node_latency_t* nl);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_PACKET_H