/* What NetHack and Lua call that a handheld has no use for, on a device:
 * running programs, the user's id, the environment's locale, a terminal to
 * read. The firmware doesn't export these; the simulator has the host's
 * (and tools/gen_names.py, which reads the names the app defines, skips
 * them). */

#if !defined(VIPER_APP_STATIC_NAME) && !defined(NH_NAMES_GENERATING)

#include <fcntl.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "nh_port.h"

/* Lua's os library */
int system(const char *command) { return command ? -1 : 0; }
char *tmpnam(char *s) {
    (void)s;
    return NULL;
}

/* Lua's io library */
FILE *freopen(const char *name, const char *mode, FILE *f) {
    (void)name;
    (void)mode;
    (void)f;
    return NULL;
}
int getchar(void) { return EOF; }

/* Lua's string library, and NetHack's number formatting */
char *setlocale(int category, const char *locale) {
    (void)category;
    (void)locale;
    return (char *)"C";
}
struct lconv *localeconv(void) {
    static struct lconv c;
    static char point[] = ".";
    static char empty[] = "";
    c.decimal_point = point;
    c.thousands_sep = empty;
    c.grouping = empty;
    return &c;
}

/* One player, no ids: NetHack's save file is <uid><name> */
uid_t getuid(void) { return 0; }
char *getcwd(char *buf, size_t size) {
    (void)buf;
    (void)size;
    return NULL;
}

/* Lock files (files.c): one game at a time anyway */
int fcntl(int fd, int cmd, ...) {
    (void)fd;
    (void)cmd;
    return 0;
}

/* creat() is open() with these flags */
int creat(const char *path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }

/* Lua's last resort: end the game, as an error would */
void abort(void) { nhv_exit(EXIT_FAILURE); }

#endif
