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
#define memset_after(obj, v, member) ({ u8* __ptr = (u8*)(obj); __typeof__(v) __val = (v); \
    memset(__ptr + offsetofend(__typeof__(*(obj)), member), __val, sizeof(*(obj)) - offsetofend(__typeof__(*(obj)), member)); })
#define memset_startat(obj, v, member) ({ u8* __ptr = (u8*)(obj); __typeof__(v) __val = (v); \
    memset(__ptr + offsetof(__typeof__(*(obj)), member), __val, sizeof(*(obj)) - offsetof(__typeof__(*(obj)), member)); })
char* kstrndup(const char* s, size_t max, gfp_t gfp);
#define kmemdup_array(src, count, element_size, gfp) kmemdup(src, (size_t)(count) * (element_size), gfp)
#define kstrdup_const kstrdup
#define kfree_const kfree
int strncasecmp(const char* a, const char* b, size_t n);
char* strsep(char** s, const char* ct);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strpbrk(const char* cs, const char* ct);
char* skip_spaces(const char* str);
char* strim(char* s);
size_t strlcat(char* dest, const char* src, size_t count);
char* strnstr(const char* s1, const char* s2, size_t len);
char* strreplace(char* s, char old, char n);
void* memscan(void* addr, int c, size_t size);
#define str_on_off(v) ((v) ? "on" : "off")
#define str_yes_no(v) ((v) ? "yes" : "no")
#define str_enabled_disabled(v) ((v) ? "enabled" : "disabled")
#define str_true_false(v) ((v) ? "true" : "false")
#define unsafe_memcpy(dst, src, bytes, justification) memcpy(dst, src, bytes)

LKPI_EXTERN_C_END

#endif
