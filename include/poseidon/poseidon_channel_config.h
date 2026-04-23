//
// Channel configuration parameters.
//

#ifndef POSEIDON_CHANNEL_CONFIG_H
#define POSEIDON_CHANNEL_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSEIDON_CHANNEL_MAX_RINGS 10

typedef struct poseidon_channel_config_t {
    uint32_t ring_sizes[POSEIDON_CHANNEL_MAX_RINGS];
    uint32_t gossip_init_interval_s;
    uint32_t gossip_steady_interval_s;
    uint32_t gossip_num_init_intervals;
    uint32_t quasar_max_hops;
    uint32_t quasar_alpha;
    uint32_t quasar_seen_size;
    uint32_t quasar_seen_hashes;
} poseidon_channel_config_t;

poseidon_channel_config_t poseidon_channel_config_defaults(void);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CHANNEL_CONFIG_H