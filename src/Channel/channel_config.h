//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_CHANNEL_CONFIG_H
#define POSEIDON_CHANNEL_CONFIG_H

#include <stdint.h>

#define POSEIDON_CHANNEL_MAX_RINGS 10

#ifdef __cplusplus
extern "C" {
#endif

typedef struct poseidon_channel_config_t {
    uint32_t ring_sizes[POSEIDON_CHANNEL_MAX_RINGS];   /**< Meridian ring sizes */
    uint32_t gossip_init_interval_s;     /**< Gossip interval during bootstrap */
    uint32_t gossip_steady_interval_s;   /**< Gossip interval in steady state */
    uint32_t gossip_num_init_intervals;  /**< Number of init-phase gossip cycles */
    uint32_t quasar_max_hops;            /**< Quasar routing filter depth */
    uint32_t quasar_alpha;               /**< Quasar random walk fan-out */
    uint32_t quasar_seen_size;           /**< Quasar dedup filter size (bits) */
    uint32_t quasar_seen_hashes;         /**< Quasar dedup filter hash count */
} poseidon_channel_config_t;

/** Returns default channel configuration values */
poseidon_channel_config_t poseidon_channel_config_defaults(void);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CHANNEL_CONFIG_H