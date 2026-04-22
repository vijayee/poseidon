//
// Created by victor on 4/22/26.
//

#include "topic_id.h"
#include "channel.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdlib.h>

int poseidon_topic_id_from_node_id(const poseidon_node_id_t* node_id,
                                    poseidon_topic_id_t* out) {
    if (node_id == NULL || out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    out->bit_depth = 256;
    strncpy(out->str, node_id->str, POSEIDON_TOPIC_ID_STR_MAX - 1);
    out->str[POSEIDON_TOPIC_ID_STR_MAX - 1] = '\0';
    memset(out->bytes, 0, 32);
    return 0;
}

int poseidon_topic_id_generate(poseidon_topic_id_t* out) {
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    out->bit_depth = 128;
    for (int i = 0; i < 16; i++) {
        out->bytes[i] = (uint8_t)(rand() & 0xFF);
    }
    out->bytes[6] = (out->bytes[6] & 0x0F) | 0x40;  // UUID v4 version
    out->bytes[8] = (out->bytes[8] & 0x3F) | 0x80;  // UUID v4 variant
    // Encode as pseudo-Base58
    static const char base58_chars[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    char* p = out->str;
    for (int i = 0; i < 16 && p < out->str + POSEIDON_TOPIC_ID_STR_MAX - 1; i++) {
        *p++ = base58_chars[out->bytes[i] % 58];
    }
    *p = '\0';
    return 0;
}

int poseidon_topic_id_from_string(const char* str, poseidon_topic_id_t* out) {
    if (str == NULL || out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    size_t len = strlen(str);
    if (len == 0 || len >= POSEIDON_TOPIC_ID_STR_MAX) return -1;
    out->bit_depth = (len >= 32) ? 256 : 128;
    strncpy(out->str, str, POSEIDON_TOPIC_ID_STR_MAX - 1);
    out->str[POSEIDON_TOPIC_ID_STR_MAX - 1] = '\0';
    return 0;
}

int poseidon_resolve_path(const struct poseidon_channel_t* channel,
                           const char* path,
                           poseidon_path_resolve_result_t* out) {
    if (path == NULL || out == NULL || channel == NULL) return -1;
    memset(out, 0, sizeof(*out));

    char path_copy[512];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char* first_component = path_copy;
    char* subtopic_start = strchr(path_copy, '/');

    if (subtopic_start != NULL) {
        *subtopic_start = '\0';
        subtopic_start++;
        strncpy(out->subtopic, subtopic_start, sizeof(out->subtopic) - 1);
        out->subtopic[sizeof(out->subtopic) - 1] = '\0';
    }

    // Try alias resolution first
    topic_alias_resolve_out_t resolve_result;
    int rc = topic_alias_resolve_ex(channel->aliases, first_component, &resolve_result);

    if (rc == 0 && resolve_result.status == TOPIC_ALIAS_RESOLVE_OK) {
        poseidon_topic_id_from_string(resolve_result.topic, &out->topic_id);
        out->found = true;
        out->ambiguous = false;
        return 0;
    }

    if (rc == 0 && resolve_result.status == TOPIC_ALIAS_RESOLVE_AMBIGUOUS) {
        poseidon_topic_id_from_string(resolve_result.candidates[0], &out->topic_id);
        out->found = true;
        out->ambiguous = true;
        return 0;
    }

    // Not an alias — treat as raw topic ID
    poseidon_topic_id_from_string(first_component, &out->topic_id);
    out->found = true;
    out->ambiguous = false;
    return 0;
}
