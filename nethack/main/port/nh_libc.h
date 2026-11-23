/* The C library functions NetHack and Lua call most, through one local
 * wrapper each (nh_libc.c), on a device. Included by nh_sim.h.
 *
 * Why: in a position-independent Xtensa ELF, every call site of a function
 * from outside the app gets a PLT relocation of its own (sprintf alone has
 * 438). Past 254 the linker splits the PLT into chunks, and binutils 2.43's
 * Xtensa code asserts on that (elf32-xtensa.c, elf_xtensa_finish_dynamic_
 * sections) for an app this size. Through a wrapper, each function needs
 * one. The simulator links the app statically, so it doesn't need them (and
 * the host's headers define some of these as macros).
 */

#ifndef NH_LIBC_H
#define NH_LIBC_H

#if !defined(VIPER_APP_STATIC_NAME) && !defined(NH_LIBC_C)

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* (declared here, as a file may have included the C library's headers
   before this one, which the names below then don't reach) */
int nhv_atoi(const char *s);
long nhv_atol(const char *s);
int nhv_fclose(FILE *f);
int nhv_fflush(FILE *f);
char *nhv_fgets(char *s, int n, FILE *f);
double nhv_floor(double x);
FILE *nhv_fopen(const char *name, const char *mode);
int nhv_fputc(int c, FILE *f);
int nhv_fputs(const char *s, FILE *f);
size_t nhv_fread(void *p, size_t size, size_t n, FILE *f);
int nhv_fseek(FILE *f, long offset, int whence);
size_t nhv_fwrite(const void *p, size_t size, size_t n, FILE *f);
char *nhv_getenv(const char *name);
struct tm *nhv_localtime(const time_t *t);
void *nhv_memchr(const void *s, int c, size_t n);
int nhv_memcmp(const void *a, const void *b, size_t n);
void *nhv_memcpy(void *to, const void *from, size_t n);
void *nhv_memmove(void *to, const void *from, size_t n);
void *nhv_memset(void *p, int c, size_t n);
time_t nhv_mktime(struct tm *t);
void nhv_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
int nhv_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int nhv_snprintf(char *buf, size_t size, const char *fmt, ...);
int nhv_sprintf(char *buf, const char *fmt, ...);
char *nhv_strcat(char *to, const char *from);
char *nhv_strchr(const char *s, int c);
int nhv_strcmp(const char *a, const char *b);
char *nhv_strcpy(char *to, const char *from);
char *nhv_strerror(int e);
size_t nhv_strlen(const char *s);
char *nhv_strncat(char *to, const char *from, size_t n);
int nhv_strncmp(const char *a, const char *b, size_t n);
char *nhv_strncpy(char *to, const char *from, size_t n);
char *nhv_strrchr(const char *s, int c);
size_t nhv_strspn(const char *s, const char *accept);
char *nhv_strstr(const char *s, const char *find);

#define atoi nhv_atoi
#define atol nhv_atol
#define fclose nhv_fclose
#define fflush nhv_fflush
#define fgets nhv_fgets
#define floor nhv_floor
#define fopen nhv_fopen
#define fputc nhv_fputc
#define fputs nhv_fputs
#define fread nhv_fread
#define fseek nhv_fseek
#define fwrite nhv_fwrite
#define getenv nhv_getenv
#define localtime nhv_localtime
#define memchr nhv_memchr
#define memcmp nhv_memcmp
#define memcpy nhv_memcpy
#define memmove nhv_memmove
#define memset nhv_memset
#define mktime nhv_mktime
#define qsort nhv_qsort
#define snprintf nhv_snprintf
#define sprintf nhv_sprintf
#define strcat nhv_strcat
#define strchr nhv_strchr
#define strcmp nhv_strcmp
#define strcpy nhv_strcpy
#define strerror nhv_strerror
#define strlen nhv_strlen
#define strncat nhv_strncat
#define strncmp nhv_strncmp
#define strncpy nhv_strncpy
#define strrchr nhv_strrchr
#define strspn nhv_strspn
#define strstr nhv_strstr
#define vsnprintf nhv_vsnprintf

#endif

#endif /* NH_LIBC_H */
