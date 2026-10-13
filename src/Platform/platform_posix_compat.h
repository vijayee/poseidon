//
// Vendored from liboffs/src/Platform/platform_posix_compat.h.
// Local modifications:
//   - #include guard renamed OFFS_PLATFORM_POSIX_COMPAT_H -> POSEIDON_PLATFORM_POSIX_COMPAT_H
//

#ifndef POSEIDON_PLATFORM_POSIX_COMPAT_H
#define POSEIDON_PLATFORM_POSIX_COMPAT_H

/*
 * Compatibility shim for POSIX types and functions missing on MSVC.
 *
 * On Windows (MSVC) the Platform layer hides the differences, but the public
 * headers still use POSIX-style types like ssize_t and call strdup/strcasecmp.
 * Define them here so the rest of the codebase compiles unchanged.
 *
 * Function declarations (no inline bodies) are provided so any TU can call
 * them as long as it includes this header. The bodies live in
 * platform_posix_compat.c and are part of the platform compatibility
 * layer, so the linker resolves them for all executables that link
 * against poseidon's platform layer.
 */

#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
  #include <BaseTsd.h>
  typedef SSIZE_T ssize_t;
  #define _SSIZE_T_DEFINED
#else
  /* POSIX consumers include this shim for isatty/STDERR_FILENO (the MSVC
     branch defines them); unistd.h is where those actually live. */
  #include <sys/types.h>
  #include <unistd.h>
#endif

#if defined(_MSC_VER)
  #include <string.h>
  #include <stdlib.h>
  #include <stdarg.h>

  /* POSIX names already available in UCRT under those exact names — nothing to do. */
  #ifndef strdup
    #define strdup _strdup
  #endif
  #ifndef strcasecmp
    #define strcasecmp _stricmp
  #endif
  #ifndef strncasecmp
    #define strncasecmp _strnicmp
  #endif
  /* UCRT ships snprintf and vsnprintf directly — do not redefine. */
  #ifndef access
    #define access _access
  #endif
  /* <unistd.h>'s access() mode constants have no UCRT equivalent. */
  #ifndef F_OK
    #define F_OK 0
  #endif
  #ifndef R_OK
    #define R_OK 4
  #endif
  #ifndef W_OK
    #define W_OK 2
  #endif
  #ifndef X_OK
    /* Accepted by _access but not enforced on Windows. */
    #define X_OK 1
  #endif
  /* UCRT's <direct.h> declares mkdir as a function; include it before the
   * mkdir macro below, or the macro breaks the header's own declaration
   * (C4003: not enough arguments for function-like macro 'mkdir'). */
  #include <direct.h>   /* _mkdir */
  #ifndef unlink
    #define unlink _unlink
  #endif
  /* POSIX mkdir takes a mode argument; _mkdir does not. */
  #ifndef mkdir
    #define mkdir(path, mode) _mkdir(path)
  #endif
  #ifndef chmod
    #define chmod _chmod
  #endif
  #include <io.h>       /* _chmod, _unlink, _access */
  #ifndef fileno
    #define fileno _fileno
  #endif
  #ifndef isatty
    #define isatty _isatty
  #endif
  /* unistd.h's stdin/stdout/stderr fileno constants have no UCRT equivalent. */
  #ifndef STDIN_FILENO
    #define STDIN_FILENO 0
  #endif
  #ifndef STDOUT_FILENO
    #define STDOUT_FILENO 1
  #endif
  #ifndef STDERR_FILENO
    #define STDERR_FILENO 2
  #endif
  #ifndef S_ISREG
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif
  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif

  /* Functions MSVC lacks outright — declared here, defined in
   * platform_posix_compat.c. TUs must include this header to call them. */
  char* strndup(const char* s, size_t n);
  char* mkdtemp(char* tmpl);
  char* realpath(const char* path, char* resolved_path);
  int asprintf(char** strp, const char* fmt, ...);
  int vasprintf(char** strp, const char* fmt, va_list ap);
  void* memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen);
#endif /* _MSC_VER */

#endif /* POSEIDON_PLATFORM_POSIX_COMPAT_H */
