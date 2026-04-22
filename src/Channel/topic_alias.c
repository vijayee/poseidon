#include "topic_alias.h"
#include "Util/allocator.h"
#include <string.h>

topic_alias_registry_t* topic_alias_registry_create(size_t capacity) {
    if (capacity == 0) capacity = 16;

    topic_alias_registry_t* reg = get_clear_memory(sizeof(topic_alias_registry_t));
    if (reg == NULL) return NULL;

    reg->entries = get_clear_memory(capacity * sizeof(topic_alias_entry_t));
    if (reg->entries == NULL) {
        free(reg);
        return NULL;
    }

    reg->capacity = capacity;
    reg->count = 0;
    platform_lock_init(&reg->lock);
    return reg;
}

void topic_alias_registry_destroy(topic_alias_registry_t* reg) {
    if (reg == NULL) return;
    if (reg->entries) free(reg->entries);
    platform_lock_destroy(&reg->lock);
    free(reg);
}

int topic_alias_register(topic_alias_registry_t* reg, const char* name, const char* topic) {
    if (reg == NULL || name == NULL || topic == NULL) return -1;

    platform_lock(&reg->lock);

    if (reg->count >= reg->capacity) {
        platform_unlock(&reg->lock);
        return -1;
    }

    strncpy(reg->entries[reg->count].name, name, TOPIC_ALIAS_MAX_NAME - 1);
    strncpy(reg->entries[reg->count].topic, topic, TOPIC_ALIAS_MAX_TOPIC - 1);
    reg->count++;

    platform_unlock(&reg->lock);
    return 0;
}

int topic_alias_unregister(topic_alias_registry_t* reg, const char* name) {
    if (reg == NULL || name == NULL) return -1;

    platform_lock(&reg->lock);
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            reg->entries[i] = reg->entries[reg->count - 1];
            reg->count--;
            platform_unlock(&reg->lock);
            return 0;
        }
    }
    platform_unlock(&reg->lock);
    return -1;
}

const char* topic_alias_resolve(const topic_alias_registry_t* reg, const char* name) {
    if (reg == NULL || name == NULL) return NULL;

    topic_alias_registry_t* mut = (topic_alias_registry_t*)reg;
    platform_lock(&mut->lock);
    const char* result = NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            result = reg->entries[i].topic;
            break;
        }
    }
    platform_unlock(&mut->lock);
    return result;
}

int topic_alias_resolve_ex(topic_alias_registry_t* reg, const char* name,
                            topic_alias_resolve_out_t* out) {
    if (reg == NULL || name == NULL || out == NULL) return -1;
    memset(out, 0, sizeof(*out));

    platform_lock(&reg->lock);

    size_t match_count = 0;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            if (match_count == 0) {
                strncpy(out->topic, reg->entries[i].topic, TOPIC_ALIAS_MAX_TOPIC - 1);
                out->topic[TOPIC_ALIAS_MAX_TOPIC - 1] = '\0';
            }
            if (match_count < TOPIC_ALIAS_MAX_CANDIDATES) {
                out->candidates[match_count] = reg->entries[i].topic;
            }
            match_count++;
        }
    }

    platform_unlock(&reg->lock);

    out->num_candidates = match_count;
    if (match_count == 0) {
        out->status = TOPIC_ALIAS_RESOLVE_NOT_FOUND;
    } else if (match_count == 1) {
        out->status = TOPIC_ALIAS_RESOLVE_OK;
    } else {
        out->status = TOPIC_ALIAS_RESOLVE_AMBIGUOUS;
    }

    return 0;
}
