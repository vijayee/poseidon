//
// Vendored from liboffs/src/Platform/platform_process.h.
// Local modifications:
//   - #include guard renamed OFFS_PLATFORM_PROCESS_H -> POSEIDON_PLATFORM_PROCESS_H
//

#ifndef POSEIDON_PLATFORM_PROCESS_H
#define POSEIDON_PLATFORM_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

int platform_getpid(void);
int platform_core_count(void);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_PLATFORM_PROCESS_H */
