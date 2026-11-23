/* The wrappers nh_libc.h sends NetHack's and Lua's C library calls through,
 * on a device: one call of each function from the app, so one PLT entry
 * each. See nh_libc.h. */

#if !defined(VIPER_APP_STATIC_NAME)

#define NH_LIBC_C /* the real names here */

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int nhv_atoi(const char *s) { return atoi(s); }
long nhv_atol(const char *s) { return atol(s); }
int nhv_fclose(FILE *f) { return fclose(f); }
int nhv_fflush(FILE *f) { return fflush(f); }
char *nhv_fgets(char *s, int n, FILE *f) { return fgets(s, n, f); }
double nhv_floor(double x) { return floor(x); }
FILE *nhv_fopen(const char *name, const char *mode) { return fopen(name, mode); }
int nhv_fputc(int c, FILE *f) { return fputc(c, f); }
int nhv_fputs(const char *s, FILE *f) { return fputs(s, f); }
size_t nhv_fread(void *p, size_t size, size_t n, FILE *f) { return fread(p, size, n, f); }
int nhv_fseek(FILE *f, long offset, int whence) { return fseek(f, offset, whence); }
size_t nhv_fwrite(const void *p, size_t size, size_t n, FILE *f) { return fwrite(p, size, n, f); }
char *nhv_getenv(const char *name) { return getenv(name); }
struct tm *nhv_localtime(const time_t *t) {
    return localtime(t);
}
void *nhv_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }
int nhv_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
void *nhv_memcpy(void *to, const void *from, size_t n) { return memcpy(to, from, n); }
void *nhv_memmove(void *to, const void *from, size_t n) { return memmove(to, from, n); }
void *nhv_memset(void *p, int c, size_t n) { return memset(p, c, n); }
time_t nhv_mktime(struct tm *t) { return mktime(t); }
void nhv_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    qsort(base, n, size, cmp);
}
int nhv_vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    return vsnprintf(buf, size, fmt, args);
}
int nhv_snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}
int nhv_sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, INT_MAX, fmt, args);
    va_end(args);
    return n;
}
char *nhv_strcat(char *to, const char *from) { return strcat(to, from); }
char *nhv_strchr(const char *s, int c) { return strchr(s, c); }
int nhv_strcmp(const char *a, const char *b) { return strcmp(a, b); }
char *nhv_strcpy(char *to, const char *from) { return strcpy(to, from); }
char *nhv_strerror(int e) { return strerror(e); }
size_t nhv_strlen(const char *s) { return strlen(s); }
char *nhv_strncat(char *to, const char *from, size_t n) { return strncat(to, from, n); }
int nhv_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
char *nhv_strncpy(char *to, const char *from, size_t n) { return strncpy(to, from, n); }
char *nhv_strrchr(const char *s, int c) { return strrchr(s, c); }
size_t nhv_strspn(const char *s, const char *accept) { return strspn(s, accept); }
char *nhv_strstr(const char *s, const char *find) { return strstr(s, find); }

#endif
