#ifndef POSEIDON_SUBTOPIC_H
#define POSEIDON_SUBTOPIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "Util/threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SUBTOPIC_MAX_PARTS 8
#define SUBTOPIC_MAX_PART_LEN 64
#define SUBTOPIC_SEPARATOR '/'

/**
 * Parses a slash-delimited subtopic path into components.
 * "Feeds/friend-only" -> ["Feeds", "friend-only"]
 *
 * @param path       Subtopic path string (null-terminated)
 * @param parts      Output array of part buffers
 * @param max_parts  Maximum number of parts to extract
 * @return           Number of parts parsed, 0 for empty path, -1 on error
 */
int subtopic_parse(const char* path, char parts[][SUBTOPIC_MAX_PART_LEN],
                   int max_parts);

/**
 * Checks if a message's subtopic matches a subscription pattern.
 * A subscription matches if it is a prefix of the message subtopic.
 * Empty subscription matches everything.
 *
 * @param message_subtopic  Subtopic of the incoming message
 * @param subscription      Subtopic the client subscribed to ("" = all)
 * @return                  true if the message should be delivered
 */
bool subtopic_matches(const char* message_subtopic, const char* subscription);

// ============================================================================
// SUBTOPIC SUBSCRIPTION TABLE
// ============================================================================

#define SUBTOPIC_TABLE_MAX_SUBS 64

typedef struct subtopic_entry_t {
    char path[SUBTOPIC_MAX_PARTS * SUBTOPIC_MAX_PART_LEN];
    uint32_t ttl;
} subtopic_entry_t;

typedef struct subtopic_table_t {
    subtopic_entry_t entries[SUBTOPIC_TABLE_MAX_SUBS];
    size_t count;
    PLATFORMLOCKTYPE(lock);
} subtopic_table_t;

subtopic_table_t* subtopic_table_create(size_t capacity);
void subtopic_table_destroy(subtopic_table_t* table);
int subtopic_table_subscribe(subtopic_table_t* table, const char* path, uint32_t ttl);
int subtopic_table_unsubscribe(subtopic_table_t* table, const char* path);
bool subtopic_table_is_subscribed(const subtopic_table_t* table, const char* path);
bool subtopic_table_should_deliver(const subtopic_table_t* table, const char* message_subtopic);
void subtopic_table_tick(subtopic_table_t* table);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_SUBTOPIC_H
