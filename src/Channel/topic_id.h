//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_TOPIC_ID_H
#define POSEIDON_TOPIC_ID_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../Crypto/node_id.h"
#include "topic_alias.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POSEIDON_TOPIC_ID_STR_MAX 48

typedef struct poseidon_topic_id_t {
    uint8_t bytes[32];
    uint16_t bit_depth;                  // 128 or 256
    char str[POSEIDON_TOPIC_ID_STR_MAX]; // Base58-encoded string
} poseidon_topic_id_t;

int poseidon_topic_id_from_node_id(const poseidon_node_id_t* node_id,
                                    poseidon_topic_id_t* out);
int poseidon_topic_id_generate(poseidon_topic_id_t* out);
int poseidon_topic_id_from_string(const char* str, poseidon_topic_id_t* out);

// ============================================================================
// PATH RESOLUTION
// ============================================================================

struct poseidon_channel_t;

typedef struct poseidon_path_resolve_result_t {
    poseidon_topic_id_t topic_id;
    char subtopic[256];
    bool found;
    bool ambiguous;
} poseidon_path_resolve_result_t;

int poseidon_resolve_path(const struct poseidon_channel_t* channel,
                           const char* path,
                           poseidon_path_resolve_result_t* out);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_TOPIC_ID_H
