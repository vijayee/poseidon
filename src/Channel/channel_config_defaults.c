//
// Standalone channel_config_defaults for the client library.
//

#include "channel_config.h"
#include <string.h>

poseidon_channel_config_t poseidon_channel_config_defaults(void) {
    poseidon_channel_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    for (int i = 0; i < POSEIDON_CHANNEL_MAX_RINGS; i++) {
        cfg.ring_sizes[i] = 8;
    }
    cfg.gossip_init_interval_s = 5;
    cfg.gossip_steady_interval_s = 30;
    cfg.gossip_num_init_intervals = 6;
    cfg.quasar_max_hops = 5;
    cfg.quasar_alpha = 3;
    cfg.quasar_seen_size = 1024;
    cfg.quasar_seen_hashes = 3;
    return cfg;
}