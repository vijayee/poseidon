//
// Vendored from liboffs/src/Platform/platform_dirs.c.
// Local modifications:
//   - rename C symbol offs_default_* -> poseidon_default_* (5 occurrences: dirs_get, dirs_t, config_path_get, plus 2 more refs)
//
#include "platform_dirs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <unistd.h>
#else
#include <unistd.h>
#endif

/* Copies src into dst, appending suffix. Returns -1 when the result would
 * not fit or src is empty, so partial results are never reported as OK. */
static int dirs_append(char* dst, size_t dst_size, const char* src,
                       const char* suffix) {
  if (src == NULL || src[0] == '\0') return -1;
  int written = snprintf(dst, dst_size, "%s%s", src, suffix);
  if (written < 0 || (size_t)written >= dst_size) return -1;
  return 0;
}

#if defined(_WIN32)

/* The node is machine state on Windows — the MSI service and an
 * interactively-run daemon share one identity, so the default state
 * always lives under the machine-wide %ProgramData%\offs. */
int poseidon_default_dirs_get(poseidon_default_dirs_t* out) {
  if (out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  char program_data[MAX_PATH];
  UINT length = GetEnvironmentVariableA("ProgramData", program_data,
                                        MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return -1;
  if (dirs_append(out->config_dir, sizeof(out->config_dir), program_data,
                  "\\offs") != 0 ||
      dirs_append(out->cache_dir, sizeof(out->cache_dir), program_data,
                  "\\offs\\cache") != 0) {
    return -1;
  }
  return 0;
}

/* $POSEIDON_CONFIG, else the same machine-wide root the daemon's default
 * state dirs live under. */
int poseidon_default_config_path_get(char* out, size_t out_size) {
  if (out == NULL || out_size == 0) return -1;
  out[0] = '\0';
  const char* from_env = getenv("POSEIDON_CONFIG");
  if (from_env != NULL && from_env[0] != '\0') {
    return dirs_append(out, out_size, from_env, "");
  }
  char program_data[MAX_PATH];
  UINT length = GetEnvironmentVariableA("ProgramData", program_data,
                                        MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return -1;
  return dirs_append(out, out_size, program_data, "\\offs\\offs.json");
}

#else

static void dirs_env_or(char* dst, size_t dst_size, const char* env_name,
                        const char* home_relative) {
  const char* value = getenv(env_name);
  if (value != NULL && value[0] != '\0' && value[0] == '/') {
    snprintf(dst, dst_size, "%s", value);
  } else {
    const char* home = getenv("HOME");
    snprintf(dst, dst_size, "%s/%s", home != NULL ? home : "",
             home_relative);
  }
}

int poseidon_default_dirs_get(poseidon_default_dirs_t* out) {
  if (out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  if (geteuid() == 0) {
#if defined(__APPLE__)
    if (dirs_append(out->config_dir, sizeof(out->config_dir),
                    "/Library/Application Support", "/offs") != 0 ||
        dirs_append(out->cache_dir, sizeof(out->cache_dir), "/Library/Caches",
                    "/offs") != 0) {
      return -1;
    }
#else
    if (dirs_append(out->config_dir, sizeof(out->config_dir), "/etc",
                    "/offs") != 0 ||
        dirs_append(out->cache_dir, sizeof(out->cache_dir), "/var/lib",
                    "/offs") != 0) {
      return -1;
    }
#endif
    return 0;
  }
  const char* home = getenv("HOME");
  if (home == NULL || home[0] == '\0') return -1;
#if defined(__APPLE__)
  if (dirs_append(out->config_dir, sizeof(out->config_dir), home,
                  "/Library/Application Support/offs") != 0 ||
      dirs_append(out->cache_dir, sizeof(out->cache_dir), home,
                  "/Library/Caches/offs") != 0) {
    return -1;
  }
#else
  dirs_env_or(out->config_dir, sizeof(out->config_dir), "XDG_CONFIG_HOME",
              ".config/offs");
  dirs_env_or(out->cache_dir, sizeof(out->cache_dir), "XDG_CACHE_HOME",
              ".cache/offs");
#endif
  return 0;
}

/* Same resolution the daemon's default-config gate performed inline —
 * factored here so the CLI and daemon cannot drift. */
int poseidon_default_config_path_get(char* out, size_t out_size) {
  if (out == NULL || out_size == 0) return -1;
  out[0] = '\0';
  const char* from_env = getenv("POSEIDON_CONFIG");
  if (from_env != NULL && from_env[0] != '\0') {
    return dirs_append(out, out_size, from_env, "");
  }
  return dirs_append(out, out_size, "/etc", "/offs/offs.json");
}

#endif