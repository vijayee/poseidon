#include "subtopic.h"
#include "Util/allocator.h"
#include <string.h>

int subtopic_parse(const char* path, char parts[][SUBTOPIC_MAX_PART_LEN],
                   int max_parts) {
    if (parts == NULL || max_parts <= 0) return -1;
    if (path == NULL) return -1;
    if (path[0] == '\0') return 0;

    int count = 0;
    const char* start = path;
    const char* p = path;

    while (*p != '\0' && count < max_parts) {
        if (*p == SUBTOPIC_SEPARATOR) {
            size_t len = (size_t)(p - start);
            if (len >= SUBTOPIC_MAX_PART_LEN) {
                return -1; // Part exceeds buffer
            }
            if (len > 0) {
                memcpy(parts[count], start, len);
                parts[count][len] = '\0';
                count++;
            }
            start = p + 1;
        }
        p++;
    }

    // Last segment
    if (*start != '\0' && count < max_parts) {
        size_t len = strlen(start);
        if (len >= SUBTOPIC_MAX_PART_LEN) {
            return -1; // Part exceeds buffer
        }
        if (len > 0) {
            memcpy(parts[count], start, len);
            parts[count][len] = '\0';
            count++;
        }
    }

    return count;
}

bool subtopic_matches(const char* message_subtopic, const char* subscription) {
    if (message_subtopic == NULL || subscription == NULL) return false;

    // Empty subscription = receive everything
    if (subscription[0] == '\0') return true;

    size_t sub_len = strlen(subscription);
    while (sub_len > 0 && subscription[sub_len - 1] == SUBTOPIC_SEPARATOR) {
        sub_len--;
    }

    size_t msg_len = strlen(message_subtopic);

    // Message must be at least as long as subscription
    if (msg_len < sub_len) return false;

    // Check prefix match
    if (strncmp(message_subtopic, subscription, sub_len) != 0) return false;

    // Exact match or subscription is a prefix followed by separator
    if (msg_len == sub_len) return true;
    if (message_subtopic[sub_len] == SUBTOPIC_SEPARATOR) return true;

    return false;
}

subtopic_table_t* subtopic_table_create(size_t capacity) {
    (void)capacity;
    subtopic_table_t* table = get_clear_memory(sizeof(subtopic_table_t));
    if (table == NULL) return NULL;
    platform_lock_init(&table->lock);
    return table;
}

void subtopic_table_destroy(subtopic_table_t* table) {
    if (table == NULL) return;
    platform_lock_destroy(&table->lock);
    free(table);
}

int subtopic_table_subscribe(subtopic_table_t* table, const char* path, uint32_t ttl) {
    if (table == NULL || path == NULL) return -1;
    platform_lock(&table->lock);

    // Update existing entry if present
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].path, path) == 0) {
            table->entries[i].ttl = ttl;
            platform_unlock(&table->lock);
            return 0;
        }
    }

    if (table->count >= SUBTOPIC_TABLE_MAX_SUBS) {
        platform_unlock(&table->lock);
        return -1;
    }

    strncpy(table->entries[table->count].path, path,
            sizeof(table->entries[table->count].path) - 1);
    table->entries[table->count].ttl = ttl;
    table->count++;
    platform_unlock(&table->lock);
    return 0;
}

int subtopic_table_unsubscribe(subtopic_table_t* table, const char* path) {
    if (table == NULL || path == NULL) return -1;
    platform_lock(&table->lock);
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].path, path) == 0) {
            table->entries[i] = table->entries[table->count - 1];
            table->count--;
            platform_unlock(&table->lock);
            return 0;
        }
    }
    platform_unlock(&table->lock);
    return -1;
}

bool subtopic_table_is_subscribed(const subtopic_table_t* table, const char* path) {
    if (table == NULL || path == NULL) return false;
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].path, path) == 0) return true;
    }
    return false;
}

bool subtopic_table_should_deliver(const subtopic_table_t* table, const char* message_subtopic) {
    if (table == NULL || message_subtopic == NULL) return false;
    for (size_t i = 0; i < table->count; i++) {
        if (subtopic_matches(message_subtopic, table->entries[i].path)) {
            return true;
        }
    }
    return false;
}

void subtopic_table_tick(subtopic_table_t* table) {
    if (table == NULL) return;
    platform_lock(&table->lock);
    size_t write = 0;
    for (size_t read = 0; read < table->count; read++) {
        if (table->entries[read].ttl > 0) {
            table->entries[read].ttl--;
            if (table->entries[read].ttl > 0) {
                if (write != read) {
                    table->entries[write] = table->entries[read];
                }
                write++;
            }
        }
    }
    table->count = write;
    platform_unlock(&table->lock);
}
