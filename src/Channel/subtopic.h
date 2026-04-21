#ifndef POSEIDON_SUBTOPIC_H
#define POSEIDON_SUBTOPIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
 * @param part_size  Size of each part buffer
 * @return           Number of parts parsed, 0 for empty path, -1 on error
 */
int subtopic_parse(const char* path, char parts[][SUBTOPIC_MAX_PART_LEN],
                   int max_parts, int part_size);

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

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_SUBTOPIC_H
