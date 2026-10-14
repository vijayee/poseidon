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

/* Create a new channel.
 * config is small (~68 bytes) so it is embedded by-value; the manager
 * copies it into the new channel's field. */
typedef struct poseidon_channel_manager_msg_create_channel_t {
    char key_type[16];
    char name[64];
    poseidon_channel_config_t config;
    poseidon_channel_t** out_channel;  /* borrowed caller-owned out-param */
} poseidon_channel_manager_msg_create_channel_t;

/* Join an existing channel by topic_id. */
typedef struct poseidon_channel_manager_msg_join_channel_t {
    char topic_id[64];
    poseidon_channel_t** out_channel;  /* borrowed caller-owned out-param */
} poseidon_channel_manager_msg_join_channel_t;

/* Destroy a channel by node_id (borrowed pointer, manager does not free). */
typedef struct poseidon_channel_manager_msg_destroy_channel_t {
    const poseidon_node_id_t* node_id;
} poseidon_channel_manager_msg_destroy_channel_t;

/* Handle a bootstrap reply. Arrays are sized to
 * POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX. */
typedef struct poseidon_channel_manager_msg_handle_bootstrap_reply_t {
    char topic_id[64];
    uint32_t responder_addr;
    uint16_t responder_port;
    uint64_t timestamp_us;
    uint32_t seed_addrs[POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX];
    uint16_t seed_ports[POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX];
    size_t num_seeds;
    int* out_rc;  /* borrowed caller-owned out-param */
} poseidon_channel_manager_msg_handle_bootstrap_reply_t;

/* Handle a bootstrap request from a peer. */
typedef struct poseidon_channel_manager_msg_handle_bootstrap_request_t {
    char topic_id[64];
    char sender_node_id[64];
    int* out_rc;  /* borrowed caller-owned out-param */
} poseidon_channel_manager_msg_handle_bootstrap_request_t;

/* Handle a verified delete notice. Caller (intercept dispatch) heap-copies the
 * notice and transfers ownership to the actor queue; the manager frees the
 * pointer after the message has been processed. */
typedef struct poseidon_channel_manager_msg_handle_delete_notice_t {
    meridian_channel_delete_notice_t* notice;  /* owned by message; freed in _impl */
} poseidon_channel_manager_msg_handle_delete_notice_t;

/* Handle a verified modify notice. Same ownership rule as delete notice. */
typedef struct poseidon_channel_manager_msg_handle_modify_notice_t {
    meridian_channel_modify_notice_t* notice;  /* owned by message; freed in _impl */
} poseidon_channel_manager_msg_handle_modify_notice_t;

/* Add a tombstone. Tombstone struct is ~280 bytes; borrowed pointer. */
typedef struct poseidon_channel_manager_msg_add_tombstone_t {
    const poseidon_tombstone_t* tombstone;  /* borrowed */
} poseidon_channel_manager_msg_add_tombstone_t;

/* Expire tombstones. No arguments. */
typedef struct poseidon_channel_manager_msg_expire_tombstones_t {
    /* intentionally empty */
    char _placeholder;  /* keep struct non-zero-sized for portability */
} poseidon_channel_manager_msg_expire_tombstones_t;

/* Register an opaque session pointer with the manager. */
typedef struct poseidon_channel_manager_msg_register_session_t {
    void* session;  /* borrowed */
} poseidon_channel_manager_msg_register_session_t;

/* Unregister an opaque session pointer. */
typedef struct poseidon_channel_manager_msg_unregister_session_t {
    void* session;  /* borrowed */
} poseidon_channel_manager_msg_unregister_session_t;

#ifdef __cplusplus
}
#endif

#endif  /* POSEIDON_CHANNEL_MANAGER_MESSAGES_H */
