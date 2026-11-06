#include <stddef.h>
#include <string.h>

void *__memcpy_chk(void *dest, const void *src, size_t n, size_t destlen) {
    (void)destlen;
    return memcpy(dest, src, n);
}

void *__memset_chk(void *dest, int c, size_t n, size_t destlen) {
    (void)destlen;
    return memset(dest, c, n);
}

void *memcpy(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memset(void *dest, int c, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    while (n--) *d++ = (unsigned char)c;
    return dest;
}
