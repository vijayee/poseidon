//
// Vendored from liboffs/src/Platform/platform_atomic.h.
// Local modifications:
//   - #include guard renamed OFFS_PLATFORM_ATOMIC_H -> POSEIDON_PLATFORM_ATOMIC_H
//

#ifndef POSEIDON_PLATFORM_ATOMIC_H
#define POSEIDON_PLATFORM_ATOMIC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Atomically write `len` bytes to `target_path`: write to a temp file in the
// same directory, fsync the file, rename over the target, fsync the directory.
// Returns 0 on success, -1 on any failure (target left untouched on failure).
// On Windows, uses MoveFileEx with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
int platform_file_atomic_write(const char* target_path, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_PLATFORM_ATOMIC_H