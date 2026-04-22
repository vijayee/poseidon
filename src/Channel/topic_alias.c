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

    // Check for duplicate
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            platform_unlock(&reg->lock);
            return -1;
        }
    }

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

    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            return reg->entries[i].topic;
        }
    }
    return NULL;
}
