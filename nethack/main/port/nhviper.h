/* NetHack's build settings for Viper. config.h includes this first (marked
 * "Viper:"), so it comes before every other NetHack header.
 *
 * NetHack is built as a Unix program, minus what a handheld lacks: no shell,
 * mail, job control, signals, compress program or chdir(). Its files are
 * found by prefix (fqn_prefix[], set by port/viper_sys.c) instead of from
 * the current directory: the data in the app's folder, the rest in its data
 * folder. The window port is port/winviper.c.
 */

#ifndef NHVIPER_H
#define NHVIPER_H

#define VIPER_PORT
#define VIPER_GRAPHICS
#define NOTTYGRAPHICS
#define DEFAULT_WINDOW_SYS "viper"

#define DLB               /* the data files packed in nhdat */
#define NOCWD_ASSUMPTIONS /* files by prefix, never by chdir() */
#define NOMAIL
#define NOSHELL
#define NOSUSPEND
#define NO_SIGNAL
#define NOCRASHREPORT
/* Each glyph's tile in glyphmap[] (src/tile.c, tools/make_tile_c.sh), for
   the tile views */
#define TILES_IN_GLYPHMAP

#ifndef NH_HOST_TOOL
#include "nh_sim.h"
#endif

#endif /* NHVIPER_H */
