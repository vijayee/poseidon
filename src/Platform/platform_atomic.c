//
// Vendored from liboffs/src/Platform/platform_atomic.c.
// Local modifications:
//   - no source changes
//
#include "platform_atomic.h"
#include "platform_file.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

int platform_file_atomic_write(const char* target_path, const uint8_t* data, size_t len) {
  if (target_path == NULL) return -1;
  if (len > 0xFFFFFFFFu) return -1;  // Windows WriteFile is 32-bit; peer-state files are tiny but guard anyway
  size_t path_len = strlen(target_path);
  // Temp path: target + ".tmp.<pid>" — same directory, unique per process.
  char tmp_path[4096];
  if (path_len + 24 >= sizeof(tmp_path)) return -1;
#ifdef _WIN32
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", target_path, (int)_getpid());
#else
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", target_path, (int)getpid());
#endif

#ifdef _WIN32
  HANDLE h = CreateFileA(tmp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_FLAG_WRITE_THROUGH, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  DWORD written = 0;
  BOOL ok = WriteFile(h, data, (DWORD)len, &written, NULL) && written == (DWORD)len;
  if (ok) ok = FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok) { DeleteFileA(tmp_path); return -1; }
  if (!MoveFileExA(tmp_path, target_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(tmp_path); return -1;
  }
  return 0;
#else
  int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0) { close(fd); unlink(tmp_path); return -1; }
    off += (size_t)n;
  }
  if (fsync(fd) < 0) { close(fd); unlink(tmp_path); return -1; }
  if (close(fd) < 0) { unlink(tmp_path); return -1; }
  if (rename(tmp_path, target_path) < 0) { unlink(tmp_path); return -1; }
  // fsync the parent directory so the rename is durable. Derive parent dir.
  // Find the last '/' in target_path; if none, skip dir fsync (cwd).
  const char* slash = strrchr(target_path, '/');
  if (slash != NULL) {
    size_t dir_len = (size_t)(slash - target_path);
    char dir_path[4096];
    if (dir_len < sizeof(dir_path)) {
      memcpy(dir_path, target_path, dir_len);
      dir_path[dir_len] = '\0';
      int dirfd = open(dir_path, O_RDONLY);
      if (dirfd >= 0) { fsync(dirfd); close(dirfd); }
    }
  }
  return 0;
#endif
}