#ifndef _LKPI_LINUX_STRING_H
#define _LKPI_LINUX_STRING_H

#include <linux/compiler.h>
#include <linux/types.h>

LKPI_EXTERN_C_BEGIN

/* Provided by the Syluxium kernel (C linkage). */
void* memcpy(void* __restrict dst, const void* __restrict src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int value, size_t n);
int memcmp(const void* a, const void* b, size_t n);

/* Provided by the LinuxKPI. */
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t max);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
int strcasecmp(const char* a, const char* b);
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
size_t strlcpy(char* dst, const char* src, size_t size);
ssize_t strscpy(char* dst, const char* src, size_t size);
char* strcat(char* dst, const char* src);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* hay, const char* needle);
void* memchr(const void* s, int c, size_t n);
char* kstrdup(const char* s, gfp_t gfp);
void* kmemdup(const void* src, size_t len, gfp_t gfp);
static inline bool str_has_prefix_lkpi(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }
#define strstarts(s, p) str_has_prefix_lkpi(s, p)
#define memzero_explicit(p, n) memset(p, 0, n)

LKPI_EXTERN_C_END

#endif
