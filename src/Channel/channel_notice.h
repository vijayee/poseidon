//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_CHANNEL_NOTICE_H
#define POSEIDON_CHANNEL_NOTICE_H

#include "channel.h"
#include "../Network/Meridian/meridian_packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates a signed delete notice for a channel.
 * Signs the notice data with the channel's private key.
 *
 * @param channel   Channel to delete (must own the key pair)
 * @return          Allocated notice, or NULL on failure
 */
meridian_channel_delete_notice_t* poseidon_channel_create_delete_notice(
    poseidon_channel_t* channel);

/**
 * Creates a signed modify notice for a channel.
 * Signs the notice data including the new config with the channel's private key.
 *
 * @param channel     Channel to modify (must own the key pair)
 * @param new_config  New configuration
 * @return            Allocated notice, or NULL on failure
 */
meridian_channel_modify_notice_t* poseidon_channel_create_modify_notice(
    poseidon_channel_t* channel,
    const poseidon_channel_config_t* new_config);

/**
 * Verifies a delete notice's signature.
 * Checks: (1) BLAKE3(public_key) == node_id, (2) signature is valid for the algorithm.
 *
 * @param notice  Notice to verify
 * @return        0 if valid, -1 if invalid
 */
int poseidon_channel_verify_delete_notice(const meridian_channel_delete_notice_t* notice);

/**
 * Verifies a modify notice's signature.
 * Checks: (1) BLAKE3(public_key) == node_id, (2) signature is valid for the algorithm.
 *
 * @param notice  Notice to verify
 * @return        0 if valid, -1 if invalid
 */
int poseidon_channel_verify_modify_notice(const meridian_channel_modify_notice_t* notice);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CHANNEL_NOTICE_H
