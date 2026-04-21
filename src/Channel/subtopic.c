#include "subtopic.h"
#include <string.h>

int subtopic_parse(const char* path, char parts[][SUBTOPIC_MAX_PART_LEN],
                   int max_parts, int part_size) {
    if (parts == NULL || max_parts <= 0 || part_size <= 0) return -1;
    if (path == NULL) return -1;
    if (path[0] == '\0') return 0;

    int count = 0;
    const char* start = path;
    const char* p = path;

    while (*p != '\0' && count < max_parts) {
        if (*p == SUBTOPIC_SEPARATOR) {
            size_t len = (size_t)(p - start);
            if (len > 0 && len < (size_t)part_size) {
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
        if (len > 0 && len < (size_t)part_size) {
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
