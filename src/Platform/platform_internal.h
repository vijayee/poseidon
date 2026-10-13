//
// Vendored from liboffs/src/Platform/platform_internal.h.
// Local modifications:
//   - #include guard renamed OFFS_PLATFORM_INTERNAL_H -> POSEIDON_PLATFORM_INTERNAL_H
//

#ifndef POSEIDON_PLATFORM_INTERNAL_H
#define POSEIDON_PLATFORM_INTERNAL_H

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>

  /* One-shot WSAStartup */
  static inline int _platform_winsock_init(void) {
    static int initialized = 0;
    if (!initialized) {
      WSADATA wsa_data;
      if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return -1;
      initialized = 1;
    }
    return 0;
  }

  /* Windows AF_UNIX detection. Winsock must be initialized before socket()
     is called, otherwise socket() fails with WSANOTINITIALISED and AF_UNIX is
     falsely reported unavailable. _platform_winsock_init() is idempotent. */
  static inline int _platform_has_af_unix(void) {
    static int checked = 0;
    static int available = 0;
    if (!checked) {
      if (_platform_winsock_init() == 0) {
        SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s != INVALID_SOCKET) {
          available = 1;
          closesocket(s);
        }
      }
      checked = 1;
    }
    return available;
  }
#endif

#endif /* POSEIDON_PLATFORM_INTERNAL_H */
