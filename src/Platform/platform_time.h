//
// Vendored from liboffs/src/Platform/platform_time.h.
// Local modifications:
//   - #include guard renamed OFFS_PLATFORM_TIME_H -> POSEIDON_PLATFORM_TIME_H
//

#ifndef POSEIDON_PLATFORM_TIME_H
#define POSEIDON_PLATFORM_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void platform_sleep_ms(unsigned int ms);
uint64_t platform_monotonic_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_PLATFORM_TIME_H */
