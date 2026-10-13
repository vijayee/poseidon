//
// Vendored from liboffs/src/Platform/platform_local.h.
// Local modifications:
//   - #include guard renamed OFFS_PLATFORM_LOCAL_H -> POSEIDON_PLATFORM_LOCAL_H
//

#ifndef POSEIDON_PLATFORM_LOCAL_H
#define POSEIDON_PLATFORM_LOCAL_H

#include "platform_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

platform_socket_t* platform_local_listen(const char* path);
platform_socket_t* platform_local_accept(platform_socket_t* listener);
platform_socket_t* platform_local_connect(const char* path);
void platform_local_cleanup(const char* path);

/* Re-arm a Windows named-pipe listener after an accept so it can accept
 * the next connection. No-op on POSIX. Returns 0 on success, -1 on error. */
int platform_local_rearm(platform_socket_t* listener);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_PLATFORM_LOCAL_H */
