//
// Vendored from liboffs/src/Platform/platform_socket_watcher.c.
// Local modifications:
//   - no source changes
//
#include "platform_socket.h"
#include "platform_socket_internal.h"

#include <poll-dancer/poll-dancer.h>

pd_watcher_t* platform_socket_watcher_create(pd_loop_t* loop,
                                             platform_socket_t* sock,
                                             pd_event_t events,
                                             pd_callback_t callback,
                                             void* user_data) {
  if (loop == NULL || sock == NULL) {
    return NULL;
  }
#ifdef _WIN32
  if (sock->is_pipe) {
    HANDLE h = (HANDLE)platform_socket_handle(sock);
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
      return NULL;
    }
    return pd_watcher_create_for_handle(loop, (void*)h, events, callback, user_data);
  }
#endif
  return pd_watcher_create(loop, platform_socket_fd(sock), events, callback, user_data);
}
