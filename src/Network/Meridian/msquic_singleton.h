//
// Created by victor on 4/21/26.
//

#ifndef POSEIDON_MSQUIC_SINGLETON_H
#define POSEIDON_MSQUIC_SINGLETON_H

#include "msquic.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gets the process-wide MsQuic API table, opening it if necessary.
 * Thread-safe. Reference-counted: each call that returns a non-NULL
 * pointer must be paired with a later poseidon_msquic_close().
 *
 * @return  QUIC_API_TABLE pointer, or NULL on failure
 */
const struct QUIC_API_TABLE* poseidon_msquic_open(void);

/**
 * Releases a reference to the process-wide MsQuic API table.
 * When the reference count drops to zero, MsQuicClose is called.
 * Must be paired with a prior poseidon_msquic_open() that succeeded.
 */
void poseidon_msquic_close(void);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MSQUIC_SINGLETON_H