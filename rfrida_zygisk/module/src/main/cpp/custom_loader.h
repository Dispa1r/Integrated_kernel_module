#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load an ELF shared library from file path without using dlopen.
   Returns a handle that can be used with custom_dlsym(). */
void *custom_dlopen(const char *path);

/* Load from a file descriptor. Uses /proc/self/fd/N internally. */
void *custom_dlopen_fd(int fd);

/* Resolve a symbol from a library loaded by custom_dlopen.
   Also searches system libraries via dlsym(RTLD_DEFAULT) as fallback. */
void *custom_dlsym(void *handle, const char *name);

/* Get the base address where the library was loaded */
uintptr_t custom_base(void *handle);

#ifdef __cplusplus
}
#endif