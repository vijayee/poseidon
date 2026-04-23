//
// Reference counting for shared ownership across threads.
//

#ifndef POSEIDON_REFCOUNTER_H
#define POSEIDON_REFCOUNTER_H

#include <stdint.h>
#include "poseidon_atomic_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refcounter_t {
    POSEIDON_ATOMIC_TYPE(uint_fast16_t) count;
    POSEIDON_ATOMIC_TYPE(uint_fast8_t) yield;
} refcounter_t;

void refcounter_init(refcounter_t* refcounter);
void refcounter_yield(refcounter_t* refcounter);
void* refcounter_reference(refcounter_t* refcounter);
void refcounter_dereference(refcounter_t* refcounter);
refcounter_t* refcounter_consume(refcounter_t** refcounter);
uint16_t refcounter_count(refcounter_t* refcounter);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_REFCOUNTER_H