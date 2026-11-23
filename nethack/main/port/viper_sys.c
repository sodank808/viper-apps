/* NetHack's platform for Viper, in place of sys/unix/unixmain.c,
 * unixunix.c, unixres.c and unixtty.c: starting a game (after
 * sys/libnh/libnhmain.c's nhmain()), where its files are, the lock file,
 * and the Unix calls the rest of NetHack expects.
 *
 * There's no chdir(): every file has a prefix (fqn_prefix[]). The data
 * (nhdat, sysconf, nethackrc) is in the app's folder; levels, saves, bones
 * and scores in its data folder.
 */

#include "hack.h" /* first: NetHack's other headers need it */

#include "dlb.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nh_port.h"

static void set_prefix(int which, const char *dir) {
    char path[BUFSZ];

    Snprintf(path, sizeof path, "%s/", dir);
    if (gf.fqn_prefix[which])
        free((genericptr_t)gf.fqn_prefix[which]);
    gf.fqn_prefix[which] = dupstr(path);
}

/* Makes a folder and any it's in that don't exist yet */
static void make_dir(const char *path) {
    char buf[BUFSZ];
    size_t len = strlen(path);

    if (len >= sizeof buf)
        return;
    Strcpy(buf, path);
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];

            buf[i] = '\0';
            (void)mkdir(buf, 0755);
            buf[i] = c;
        }
    }
}

void nhv_nethack_main(const char *app_dir, const char *data_dir, const char *name) {
    static char *argv[] = {(char *)"nethack", NULL};
    char path[BUFSZ];
    NHFILE *nhfp;
    boolean resuming = FALSE;

    early_init(1, argv);
    gh.hname = argv[0];
    svh.hackpid = 1;
    choose_windows(DEFAULT_WINDOW_SYS);

    /* Where everything is */
    set_prefix(HACKPREFIX, data_dir);
    set_prefix(LEVELPREFIX, data_dir);
    set_prefix(SAVEPREFIX, data_dir);
    set_prefix(BONESPREFIX, data_dir);
    set_prefix(SCOREPREFIX, data_dir);
    set_prefix(LOCKPREFIX, data_dir);
    set_prefix(TROUBLEPREFIX, data_dir);
    set_prefix(CONFIGPREFIX, data_dir);
    set_prefix(DATAPREFIX, app_dir);
    set_prefix(SYSCONFPREFIX, app_dir);
    Snprintf(path, sizeof path, "%s/save", data_dir);
    make_dir(data_dir);
    make_dir(path);
    /* The options the app ships with, not the user's ~/.nethackrc */
    Snprintf(path, sizeof path, "%s/nethackrc", app_dir);
    gc.cmdline_rcfile = dupstr(path);

    initoptions();
    u.uhp = 1; /* prevent RIP on early quits */
    program_state.preserve_locks = 1;
    init_nhwindows(NULL, argv);

    /* One hero, named by the app: its save file is found by this name */
    (void)strncpy(svp.plname, name, sizeof svp.plname - 1);
    svp.plname[sizeof svp.plname - 1] = '\0';
    set_playmode();
    gp.plnamelen = (int)strlen(svp.plname);
    plnamesuffix();

    dlb_init();
    vision_init();
    init_sound_disp_gamewindows();

    getlock();
    program_state.preserve_locks = 0;
    if ((nhfp = restore_saved_game()) != 0) {
        pline("Restoring save file...");
        mark_synch();
        if (dorecover(nhfp))
            resuming = TRUE;
    }
    if (!resuming) {
        player_selection();
        newgame();
    }
    moveloop(resuming);
    nh_terminate(EXIT_SUCCESS);
}

/* ---- the lock file (unixunix.c) ---- */

/* One hero, one game: a lock file left behind is from a game that didn't
   end (the app was stopped without saving), so its levels go */
void getlock(void) {
    const char *fq_lock;
    int fd;

    Sprintf(gl.lock, "0%s", svp.plname);
    regularize(gl.lock);
    set_levelfile_name(gl.lock, 0);
    fq_lock = fqname(gl.lock, LEVELPREFIX, 0);
    if (access(fq_lock, F_OK) == 0) {
        for (int i = 1; i <= MAXDUNGEON * MAXLEVEL + 1; i++)
            delete_levelfile(i);
        delete_levelfile(0);
    }
    fd = open(fq_lock, O_WRONLY | O_CREAT | O_TRUNC, FCMASK);
    if (fd < 0)
        error("Can't create the lock file %s (%d).", fq_lock, errno);
    (void)write(fd, (genericptr_t)&svh.hackpid, sizeof svh.hackpid);
    (void)close(fd);
}

void regularize(char *s) {
    char *lp;

    while ((lp = strchr(s, '.')) != 0 || (lp = strchr(s, '/')) != 0 || (lp = strchr(s, ' ')) != 0)
        *lp = '_';
}

void ask_about_panic_save(void) {}

#if defined(TIMED_DELAY) && !defined(msleep) && defined(SYSV)
void msleep(unsigned msec UNUSED) {}
#endif

/* ---- unixmain.c ---- */

void sethanguphandler(void (*handler)(int) UNUSED) {}

boolean authorize_wizard_mode(void) { return FALSE; }

boolean authorize_explore_mode(void) { return TRUE; }

void append_slash(char *name) {
    size_t len = strlen(name);

    if (len && name[len - 1] != '/')
        Strcat(name, "/");
}

boolean check_user_string(const char *optstr UNUSED) { return FALSE; }

/* The hero's name comes from the app (nhv_nethack_main()) */
boolean whoami(void) { return FALSE; }

char *get_login_name(void) {
    static char name[] = "viper";

    return name;
}

unsigned long sys_random_seed(void) {
    unsigned long seed = nhv_random_seed(); /* the tests' own, if they set one */

    return seed ? seed : (unsigned long)time((time_t *)0) ^ (unsigned long)svh.hackpid;
}

void get_nhuuid(void) {}

void free_nhuuid(void) {}

/* ---- unixtty.c ---- */

void gettty(void) {}

void settty(const char *s) {
    if (s && *s)
        raw_print(s);
}

void setftty(void) {}

void intron(void) {}

void introff(void) {}

void error(const char *s, ...) {
    char buf[BUFSZ];
    va_list args;

    va_start(args, s);
    (void)vsnprintf(buf, sizeof buf, s, args);
    va_end(args);
    if (*buf)
        nhv_log(buf);
    nhv_exit(EXIT_FAILURE);
}
