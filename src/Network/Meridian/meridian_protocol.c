//
// Created by victor on 4/19/26.
//

#include "../../Util/threadding.h"
#include "meridian_protocol.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

meridian_protocol_t* meridian_protocol_create(const meridian_protocol_config_t* config) {
    if (config == NULL) return NULL;

    meridian_protocol_t* protocol = (meridian_protocol_t*)
        get_clear_memory(sizeof(meridian_protocol_t));

    protocol->config.listen_port = config->listen_port;
    protocol->config.info_port = config->info_port;
    protocol->config.primary_ring_size = config->primary_ring_size;
    protocol->config.secondary_ring_size = config->secondary_ring_size;
    protocol->config.ring_exponent_base = config->ring_exponent_base;
    protocol->config.init_gossip_interval_s = config->init_gossip_interval_s;
    protocol->config.num_init_gossip_intervals = config->num_init_gossip_intervals;
    protocol->config.steady_state_gossip_interval_s = config->steady_state_gossip_interval_s;
    protocol->config.replace_interval_s = config->replace_interval_s;
    protocol->config.gossip_timeout_ms = config->gossip_timeout_ms;
    protocol->config.measure_timeout_ms = config->measure_timeout_ms;
    protocol->config.pool = config->pool;
    protocol->config.wheel = config->wheel;

    protocol->state = MERIDIAN_PROTOCOL_STATE_INIT;
    protocol->socket_fd = -1;
    protocol->running = false;

    protocol->ring_set = meridian_ring_set_create(
        config->primary_ring_size,
        config->secondary_ring_size,
        config->ring_exponent_base
    );

    protocol->rendv_handle = meridian_rendv_handle_create();
    protocol->latency_cache = meridian_latency_cache_create(MERIDIAN_PROBE_CACHE_SIZE);

    protocol->num_seed_nodes = 0;
    protocol->num_connected_peers = 0;

    platform_lock_init(&protocol->lock);
    refcounter_init(&protocol->refcounter);

    return protocol;
}

void meridian_protocol_destroy(meridian_protocol_t* protocol) {
    if (protocol == NULL) return;

    refcounter_dereference(&protocol->refcounter);
    if (refcounter_count(&protocol->refcounter) == 0) {
        if (protocol->running) {
            meridian_protocol_stop(protocol);
        }

        if (protocol->socket_fd >= 0) {
            close(protocol->socket_fd);
            protocol->socket_fd = -1;
        }

        if (protocol->ring_set) {
            meridian_ring_set_destroy(protocol->ring_set);
        }

        if (protocol->rendv_handle) {
            meridian_rendv_handle_destroy(protocol->rendv_handle);
        }

        if (protocol->latency_cache) {
            meridian_latency_cache_destroy(protocol->latency_cache);
        }

        for (size_t i = 0; i < protocol->num_seed_nodes; i++) {
            if (protocol->seed_nodes[i]) {
                refcounter_dereference(&protocol->seed_nodes[i]->refcounter);
                if (refcounter_count(&protocol->seed_nodes[i]->refcounter) == 0) {
                    free(protocol->seed_nodes[i]);
                }
            }
        }

        for (size_t i = 0; i < protocol->num_connected_peers; i++) {
            if (protocol->connected_peers[i]) {
                refcounter_dereference(&protocol->connected_peers[i]->refcounter);
                if (refcounter_count(&protocol->connected_peers[i]->refcounter) == 0) {
                    free(protocol->connected_peers[i]);
                }
            }
        }

        if (protocol->gossip_handle) {
            meridian_gossip_handle_destroy(protocol->gossip_handle);
            protocol->gossip_handle = NULL;
        }

        platform_lock_destroy(&protocol->lock);
        free(protocol);
    }
}

int meridian_protocol_start(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    platform_lock(&protocol->lock);

    if (protocol->running) {
        platform_unlock(&protocol->lock);
        return 0;
    }

    protocol->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (protocol->socket_fd < 0) {
        platform_unlock(&protocol->lock);
        return -1;
    }

    int opt = 1;
    setsockopt(protocol->socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&protocol->local_addr, 0, sizeof(protocol->local_addr));
    protocol->local_addr.sin_family = AF_INET;
    protocol->local_addr.sin_addr.s_addr = INADDR_ANY;
    protocol->local_addr.sin_port = htons(protocol->config.listen_port);

    if (bind(protocol->socket_fd, (struct sockaddr*)&protocol->local_addr,
             sizeof(protocol->local_addr)) < 0) {
        close(protocol->socket_fd);
        protocol->socket_fd = -1;
        platform_unlock(&protocol->lock);
        return -1;
    }

    meridian_gossip_config_t gossip_config = {
        .user_ctx = protocol,
        .outbound_cb = NULL,
        .completed_cb = NULL,
        .init_interval_s = protocol->config.init_gossip_interval_s,
        .num_init_intervals = protocol->config.num_init_gossip_intervals,
        .steady_state_interval_s = protocol->config.steady_state_gossip_interval_s,
        .timeout_ms = protocol->config.gossip_timeout_ms
    };
    protocol->gossip_handle = meridian_gossip_handle_create(&gossip_config);

    if (protocol->gossip_handle == NULL) {
        close(protocol->socket_fd);
        protocol->socket_fd = -1;
        platform_unlock(&protocol->lock);
        return -1;
    }

    protocol->state = MERIDIAN_PROTOCOL_STATE_BOOTSTRAPPING;
    protocol->running = true;

    platform_unlock(&protocol->lock);
    return 0;
}

int meridian_protocol_stop(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    platform_lock(&protocol->lock);

    if (!protocol->running) {
        platform_unlock(&protocol->lock);
        return 0;
    }

    protocol->running = false;
    protocol->state = MERIDIAN_PROTOCOL_STATE_SHUTTING_DOWN;

    if (protocol->gossip_handle) {
        meridian_gossip_handle_stop(protocol->gossip_handle);
    }

    platform_unlock(&protocol->lock);
    return 0;
}

int meridian_protocol_add_seed_node(meridian_protocol_t* protocol,
                                     uint32_t addr, uint16_t port) {
    if (protocol == NULL) return -1;
    if (protocol->num_seed_nodes >= 16) return -1;

    meridian_node_t* node = meridian_node_create(addr, port);
    if (node == NULL) return -1;

    protocol->seed_nodes[protocol->num_seed_nodes++] = node;
    return 0;
}

int meridian_protocol_connect(meridian_protocol_t* protocol,
                               uint32_t addr, uint16_t port) {
    if (protocol == NULL) return -1;
    if (protocol->num_connected_peers >= 64) return -1;

    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->connected_peers[i]->addr == addr &&
            protocol->connected_peers[i]->port == port) {
            return 0;
        }
    }

    meridian_node_t* node = meridian_node_create(addr, port);
    if (node == NULL) return -1;

    protocol->connected_peers[protocol->num_connected_peers++] = node;
    return 0;
}

int meridian_protocol_disconnect(meridian_protocol_t* protocol,
                                  uint32_t addr, uint16_t port) {
    if (protocol == NULL) return -1;

    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->connected_peers[i]->addr == addr &&
            protocol->connected_peers[i]->port == port) {

            refcounter_dereference(&protocol->connected_peers[i]->refcounter);
            if (refcounter_count(&protocol->connected_peers[i]->refcounter) == 0) {
                free(protocol->connected_peers[i]);
            }

            for (size_t j = i; j < protocol->num_connected_peers - 1; j++) {
                protocol->connected_peers[j] = protocol->connected_peers[j + 1];
            }
            protocol->num_connected_peers--;
            return 0;
        }
    }

    return -1;
}

int meridian_protocol_send_packet(meridian_protocol_t* protocol,
                                   const uint8_t* data, size_t len,
                                   const meridian_node_t* target) {
    if (protocol == NULL || data == NULL || target == NULL) return -1;
    if (protocol->socket_fd < 0) return -1;

    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_addr.s_addr = target->addr;
    target_addr.sin_port = target->port;

    ssize_t sent = sendto(protocol->socket_fd, data, len, 0,
                          (struct sockaddr*)&target_addr, sizeof(target_addr));

    return (sent == (ssize_t)len) ? 0 : -1;
}

int meridian_protocol_broadcast(meridian_protocol_t* protocol,
                                 const uint8_t* data, size_t len) {
    if (protocol == NULL || data == NULL) return -1;

    int result = 0;
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (meridian_protocol_send_packet(protocol, data, len,
                                         protocol->connected_peers[i]) != 0) {
            result = -1;
        }
    }
    return result;
}

meridian_node_t* meridian_protocol_find_closest(meridian_protocol_t* protocol,
                                                uint32_t target_addr, uint16_t target_port) {
    if (protocol == NULL) return NULL;

    return meridian_ring_set_find_closest(protocol->ring_set, target_addr, target_port);
}

meridian_node_t** meridian_protocol_get_connected_peers(meridian_protocol_t* protocol,
                                                      size_t* num_peers) {
    if (protocol == NULL || num_peers == NULL) return NULL;

    *num_peers = protocol->num_connected_peers;
    return protocol->connected_peers;
}

int meridian_protocol_gossip(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    bool should_gossip = false;
    if (protocol->gossip_handle) {
        meridian_gossip_scheduler_t* sched = protocol->gossip_handle->scheduler;
        if (sched) {
            meridian_gossip_scheduler_tick(sched, &should_gossip);
        }
    }

    if (should_gossip && protocol->num_connected_peers > 0) {
        meridian_gossip_handle_send_gossip(protocol->gossip_handle,
                                           protocol->connected_peers,
                                           protocol->num_connected_peers);
    }

    return 0;
}

int meridian_protocol_ring_management(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    for (int ring = 0; ring < MERIDIAN_MAX_RINGS; ring++) {
        if (meridian_ring_set_eligible_for_replacement(protocol->ring_set, ring)) {

        }
    }

    return 0;
}

int meridian_protocol_on_packet(meridian_protocol_t* protocol,
                                 const uint8_t* data, size_t len,
                                 const meridian_node_t* from) {
    if (protocol == NULL || data == NULL || from == NULL) return -1;

    struct cbor_load_result result;
    cbor_item_t* item = cbor_load(data, len, &result);
    if (item == NULL) return -1;

    cbor_decref(&item);
    return 0;
}

int meridian_protocol_on_measure_result(meridian_protocol_t* protocol,
                                         const meridian_measure_result_t* result) {
    if (protocol == NULL || result == NULL) return -1;

    if (result->success && result->node) {
        meridian_latency_cache_insert(protocol->latency_cache,
                                      result->node, result->latency_us);
    }

    return 0;
}

int meridian_protocol_set_callbacks(meridian_protocol_t* protocol,
                                     const meridian_protocol_callbacks_t* callbacks) {
    (void) protocol;
    (void) callbacks;
    return 0;
}

int meridian_protocol_get_local_node(meridian_protocol_t* protocol,
                                      uint32_t* addr, uint16_t* port) {
    if (protocol == NULL) return -1;

    if (addr) *addr = protocol->local_addr.sin_addr.s_addr;
    if (port) *port = ntohs(protocol->local_addr.sin_port);

    return 0;
}