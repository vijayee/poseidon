//
// channel_manager_messages.h
// Poseidon subsystem-actor payload layouts for poseidon_channel_manager_t.
//
// One struct per POSEIDON_CHANNEL_MANAGER_MSG_* enum in Actor/message.h. The
// actor's dispatch switch selects on the enum, casts void* payload to the
// matching struct, and reads fields. Out-parameters (e.g. poseidon_channel_t*,
// int rc) are borrowed caller-owned pointers filled by the handler before the
// actor returns control via actor_run's synchronous drain.
//
// Mirrors the meridian_conn_messages.h split that keeps Actor/message.h free
// of subsystem dependencies.
//

#ifndef POSEIDON_CHANNEL_MANAGER_MESSAGES_H
#define POSEIDON_CHANNEL_MANAGER_MESSAGES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../Crypto/node_id.h"
#include "../Actor/actor_reply.h"
#include "channel.h"
#include "channel_config.h"
#include "../Network/Meridian/meridian_packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatch table (enum -> handler, payload, return contract):
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_CREATE_CHANNEL
 *     -> handler:        create_channel
 *        payload:        poseidon_channel_manager_msg_create_channel_t
 *        return:         out_channel written; out_channel==NULL on failure
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_JOIN_CHANNEL
 *     -> handler:        join_channel
 *        payload:        poseidon_channel_manager_msg_join_channel_t
 *        return:         out_channel written; out_channel==NULL on failure
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_DESTROY_CHANNEL
 *     -> handler:        destroy_channel
 *        payload:        poseidon_channel_manager_msg_destroy_channel_t
 *        return:         void
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_BOOTSTRAP_REPLY
 *     -> handler:        handle_bootstrap_reply
 *        payload:        poseidon_channel_manager_msg_handle_bootstrap_reply_t
 *        return:         out_rc written; 0 success, -1 not-found
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_BOOTSTRAP_REQUEST
 *     -> handler:        handle_bootstrap_request
 *        payload:        poseidon_channel_manager_msg_handle_bootstrap_request_t
 *        return:         out_rc written; 0 success, -1 not-member
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_DELETE_NOTICE
 *     -> handler:        handle_delete_notice
 *        payload:        poseidon_channel_manager_msg_handle_delete_notice_t
 *        return:         void
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_MODIFY_NOTICE
 *     -> handler:        handle_modify_notice
 *        payload:        poseidon_channel_manager_msg_handle_modify_notice_t
 *        return:         void
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_ADD_TOMBSTONE
 *     -> handler:        add_tombstone
 *        payload:        poseidon_channel_manager_msg_add_tombstone_t
 *        return:         void (rc dropped, internal push always succeeds)
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_EXPIRE_TOMBSTONES
 *     -> handler:        expire_tombstones
 *        payload:        poseidon_channel_manager_msg_expire_tombstones_t
 *        return:         void
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_REGISTER_SESSION
 *     -> handler:        register_session
 *        payload:        poseidon_channel_manager_msg_register_session_t
 *        return:         void
 *
 *   POSEIDON_CHANNEL_MANAGER_MSG_UNREGISTER_SESSION
 *     -> handler:        unregister_session
 *        payload:        poseidon_channel_manager_msg_unregister_session_t
 *        return:         void
 */

/* Every request the manager receives is a heap-allocated shell whose FIRST
 * member is the reply block: the wrapper waits on the block's condition
 * variable until the actor has written the answer in, and the actor hands
 * the request back. A request owns everything it carries, because the actor
 * may still be holding it after the caller has given up and walked away —
 * so nothing in a request may point at the caller's stack.
 *
 * The requests that need no answer (the session registry, the tombstone
 * sweep, the notice handlers) still carry the reply block: the wrapper
 * hands them over and moves on, and the block is what lets the actor
 * release the shell when it is done with it.
 */

/* Create a new channel. */
typedef struct poseidon_channel_manager_msg_create_channel_t {
    actor_reply_t reply;   /**< Must stay first. */
    char key_type[16];
    char name[64];
    poseidon_channel_config_t config;
    poseidon_channel_t* out_channel;  /**< The actor writes the new channel
                                           here; NULL on rejection. */
} poseidon_channel_manager_msg_create_channel_t;

/* Join an existing channel via bootstrap request. */
typedef struct poseidon_channel_manager_msg_join_channel_t {
    actor_reply_t reply;   /**< Must stay first. */
    char topic_id[64];
    poseidon_channel_t* out_channel;  /**< The actor writes the joined
                                           channel here; NULL on rejection. */
} poseidon_channel_manager_msg_join_channel_t;

/* Tear down a channel identified by node_id. */
typedef struct poseidon_channel_manager_msg_destroy_channel_t {
    actor_reply_t reply;   /**< Must stay first. */
    poseidon_node_id_t node_id;  /**< Copied by value: the request outlives
                                      the caller's frame. */
    int result;            /**< The actor writes the outcome here. */
} poseidon_channel_manager_msg_destroy_channel_t;

/* Handle a bootstrap reply. Arrays are sized to
 * POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX. */
typedef struct poseidon_channel_manager_msg_handle_bootstrap_reply_t {
    actor_reply_t reply;   /**< Must stay first. */
    char topic_id[64];
    uint32_t responder_addr;
    uint16_t responder_port;
    uint64_t timestamp_us;
    uint32_t seed_addrs[POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX];
    uint16_t seed_ports[POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX];
    size_t num_seeds;
    bool out_first_reply;      /**< The actor writes the flag here. */
    meridian_protocol_t* out_protocol;  /**< The actor writes the protocol
                                             here when this is the first
                                             reply. */
    int result;                /**< The actor writes the outcome here. */
} poseidon_channel_manager_msg_handle_bootstrap_reply_t;

/* Handle a bootstrap request from a peer. */
typedef struct poseidon_channel_manager_msg_handle_bootstrap_request_t {
    actor_reply_t reply;   /**< Must stay first. */
    char topic_id[64];
    char sender_node_id[64];
    char responder_node_id[64];   /**< The actor writes the responder here. */
    uint32_t responder_addr;      /**< The actor writes it here. */
    uint16_t responder_port;      /**< The actor writes it here. */
    poseidon_channel_t* dial_channel;  /**< The actor writes it here. */
    uint64_t timestamp_us;        /**< The actor writes it here. */
    unsigned char* publish_buf;    /**< The actor's reply bytes; the caller
                                       publishes them and frees them. */
    size_t publish_len;            /**< The actor writes the length here. */
    cbor_item_t* tombstone_encoded;  /**< The actor's encoded tombstone
                                           notice; the caller publishes it
                                           and decrefs it. */
    int tombstone_type;            /**< The actor writes it here. */
    int result;                    /**< The actor writes the outcome here. */
} poseidon_channel_manager_msg_handle_bootstrap_request_t;

/* Handle a verified delete notice. */
typedef struct poseidon_channel_manager_msg_handle_delete_notice_t {
    actor_reply_t reply;   /**< Must stay first. */
    meridian_channel_delete_notice_t* notice;  /**< Owned by the request. */
    bool did_work;         /**< The actor writes the flag here: the notice
                               produced a tombstone. */
} poseidon_channel_manager_msg_handle_delete_notice_t;

/* Handle a verified modify notice. */
typedef struct poseidon_channel_manager_msg_handle_modify_notice_t {
    actor_reply_t reply;   /**< Must stay first. */
    meridian_channel_modify_notice_t* notice;  /**< Owned by the request. */
    bool did_work;         /**< The actor writes the flag here: the notice
                               produced a tombstone. */
} poseidon_channel_manager_msg_handle_modify_notice_t;

/* Add a tombstone. The tombstone struct is ~280 bytes; copied by value. */
typedef struct poseidon_channel_manager_msg_add_tombstone_t {
    actor_reply_t reply;   /**< Must stay first. */
    poseidon_tombstone_t tombstone;  /**< Copied by value. */
    int result;            /**< The actor writes the outcome here. */
} poseidon_channel_manager_msg_add_tombstone_t;

/* Expire expired tombstones. */
typedef struct poseidon_channel_manager_msg_expire_tombstones_t {
    actor_reply_t reply;   /**< Must stay first. */
} poseidon_channel_manager_msg_expire_tombstones_t;

/* Register a client session in the manager's session registry. */
typedef struct poseidon_channel_manager_msg_register_session_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* session;  /**< Borrowed; the registry does not take ownership. */
    int result;     /**< The actor writes the outcome here. */
} poseidon_channel_manager_msg_register_session_t;

/* Unregister a client session. */
typedef struct poseidon_channel_manager_msg_unregister_session_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* session;  /**< Borrowed; the registry does not take ownership. */
    int result;     /**< The actor writes the outcome here. */
} poseidon_channel_manager_msg_unregister_session_t;

#ifdef __cplusplus
}
#endif

#endif  /* POSEIDON_CHANNEL_MANAGER_MESSAGES_H */
