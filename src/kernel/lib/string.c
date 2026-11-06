#include <string.h>
#include <strings.h>
#include <stddef.h>

int tolower(int c);
int toupper(int c);

#pragma GCC optimize("no-tree-loop-distribute-patterns")

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

void *memset(void *dest, int c, size_t n) {
    unsigned char *d = dest;
    unsigned char v = (unsigned char)c;
    while (n--) *d++ = v;
    return dest;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    unsigned char v = (unsigned char)c;
    while (n--) {
        if (*p == v) return (void *)p;
        p++;
    }
    return NULL;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a;
    const unsigned char *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++)) {
    }
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = 0;
    return dest;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch) last = s;
        if (!*s) break;
    }
    return (char *)last;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n]) {
        const char *r = reject;
        while (*r) {
            if (s[n] == *r) return n;
            r++;
        }
        n++;
    }
    return n;
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *a = hay;
        const char *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) return (char *)hay;
    }
    return NULL;
}

char *strcat(char *dest, const char *src) {
    strcpy(dest + strlen(dest), src);
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest + strlen(dest);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = 0;
    return dest;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
        n--;
    }
    if (!n) return 0;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

int toupper(int c) {
    if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    return c;
}
