#ifndef POSEIDON_MSQUIC_H
#define POSEIDON_MSQUIC_H

#include <msquic.h>
#include <stdbool.h>

/* A C++ translation unit that reaches this header sees C++ linkage
 * otherwise, and asks the linker for mangled symbols the library does
 * not define. The same guard the actor, message, message-queue and
 * scheduler headers carry. */
#ifdef __cplusplus
extern "C" {
#endif

// Process-wide reference-counted wrapper around the MSQUIC API table and
// per-app-identity registrations.
//
// MsQuicOpen2 may only be called once per process; RegistrationOpen may be
// called at most once per distinct app identity per process. Callers must
// therefore route every access through these helpers rather than calling the
// raw API.
//
// Lifetime:
//   - poseidon_msquic_open() returns the process-wide QUIC_API_TABLE pointer
//     and increments the API table refcount.
//   - poseidon_msquic_close() decrements that refcount; the last close calls
//     MsQuicClose.
//   - poseidon_msquic_open_registration() returns a per-app-name HQUIC
//     registration and increments the per-app-name refcount. The first call
//     for a given app name calls RegistrationOpen; subsequent calls return the
//     existing handle.
//   - poseidon_msquic_close_registration() decrements the per-app-name
//     refcount; the last close for that name calls RegistrationClose and the
//     slot is removed.

const struct QUIC_API_TABLE* poseidon_msquic_open(void);
void poseidon_msquic_close(void);

bool poseidon_msquic_open_registration(const char* app_name, HQUIC* out_reg);
void poseidon_msquic_close_registration(const char* app_name);

/**
 * Returns the process-wide MSQUIC API table pointer. The pointer is valid as
 * long as at least one caller holds a refcount (via poseidon_msquic_open or
 * poseidon_msquic_open_registration). Callers must not cache it across
 * teardown.
 */
const struct QUIC_API_TABLE* poseidon_msquic_api(void);



#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_MSQUIC_H */
