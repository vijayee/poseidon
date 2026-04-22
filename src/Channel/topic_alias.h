#ifndef POSEIDON_TOPIC_ALIAS_H
#define POSEIDON_TOPIC_ALIAS_H

#include <stdint.h>
#include <stddef.h>
#include "Util/threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOPIC_ALIAS_MAX_NAME 64
#define TOPIC_ALIAS_MAX_TOPIC 64

typedef struct topic_alias_entry_t {
    char name[TOPIC_ALIAS_MAX_NAME];
    char topic[TOPIC_ALIAS_MAX_TOPIC];
} topic_alias_entry_t;

typedef struct topic_alias_registry_t {
    topic_alias_entry_t* entries;
    size_t capacity;
    size_t count;
    PLATFORMLOCKTYPE(lock);
} topic_alias_registry_t;

topic_alias_registry_t* topic_alias_registry_create(size_t capacity);
void topic_alias_registry_destroy(topic_alias_registry_t* reg);
int topic_alias_register(topic_alias_registry_t* reg, const char* name, const char* topic);
int topic_alias_unregister(topic_alias_registry_t* reg, const char* name);
const char* topic_alias_resolve(const topic_alias_registry_t* reg, const char* name);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_TOPIC_ALIAS_H
