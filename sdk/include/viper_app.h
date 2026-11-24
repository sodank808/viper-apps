#pragma once

/*
 * What an ELF app tells Viper about itself, so it can run as an app on the home
 * screen: its app_main() returns a viper_app_t, and the firmware's ElfApp
 * (components/system/src/elf_app.cpp) calls its callbacks the way the app
 * manager calls a built-in app's IAppLifecycle methods.
 *
 *     static void create(void *container) { ... }
 *     static uint32_t loop(const viper_input_t *now, const viper_input_t *previous) { ... }
 *
 *     static const viper_app_t app = {
 *         .magic = VIPER_APP_MAGIC,
 *         .abi_version = VIPER_APP_ABI_VERSION,
 *         .render_engine = VIPER_RENDER_LVGL,
 *         .create = create,
 *         .loop = loop,
 *     };
 *     VIPER_APP(app)
 *
 * Plain C, shared by apps and the firmware.
 *
 * Compatibility. Installed apps outlive the firmware they were built with: an
 * update replaces the firmware and leaves the apps, and a rollback puts older
 * firmware under apps installed since. So:
 *
 * - Adding is free. A new function (a line in components/elf_loader/exports/),
 *   or a field at the end of a struct that carries its `size`, needs no new
 *   version: old apps don't use it, and firmware without it refuses an app that
 *   calls a function it doesn't export ("needs <name>, which the firmware
 *   doesn't export") before running any of it.
 * - Nothing is taken away or changed in place: not an exported function, its
 *   arguments or what it does, nor a struct field or a constant's value.
 *   components/elf_loader/exports.lock holds the names
 *   (tool/check_exports_lock.py).
 * - What can't be done by adding (viper_app_t itself, or what a callback gets
 *   or means) takes a new VIPER_APP_ABI_VERSION. The firmware then keeps
 *   running the versions before it, back to VIPER_APP_ABI_MIN_VERSION: ElfApp
 *   (components/system/src/elf_app.cpp) treats an app by the abi_version it
 *   declares. Raise the minimum only when an old version can't be kept, since
 *   every installed app built for it stops running.
 *
 * The firmware refuses an app outside that range, saying whether the app or the
 * firmware is the older one (components/system/include/elf_app_abi.hpp).
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIPER_APP_MAGIC 0x50504156u /* "VAPP" */
/* The version apps built with this header declare, and the newest the firmware
 * built with it runs. 2: loop() gets viper_input_t */
#define VIPER_APP_ABI_VERSION 2u
/* The oldest version the firmware built with this header still runs. Version 1
 * (loop() got viper_gamepad_t) isn't run any more. */
#define VIPER_APP_ABI_MIN_VERSION 2u

/* The app API level: one more each time the firmware exports new functions.
 * Apps built with this header can call the functions of every level up to it;
 * firmware with a lower level refuses an app that calls one it doesn't have,
 * naming it. components/elf_loader/exports.lock lists the level each function
 * arrived in, check_app_elf.py reports the level an app needs, and its
 * installed info.json says so as "min_api". viper_api_level() is the running
 * firmware's. Written by check_exports_lock.py --update; don't edit. */
#define VIPER_API_LEVEL 6

typedef enum {
    VIPER_RENDER_LVGL = 0,       /* build an LVGL UI under the container */
    VIPER_RENDER_VIDEO_BLIT = 1, /* draw to the framebuffer (viper_frame_begin()) */
} viper_render_engine_t;

/* Buttons, as bits of viper_input_t.buttons */
#define VIPER_BUTTON_A (1u << 0)
#define VIPER_BUTTON_B (1u << 1)
#define VIPER_BUTTON_X (1u << 2)
#define VIPER_BUTTON_Y (1u << 3)
#define VIPER_BUTTON_SELECT (1u << 4)
#define VIPER_BUTTON_START (1u << 5)
#define VIPER_BUTTON_UP (1u << 6)
#define VIPER_BUTTON_DOWN (1u << 7)
#define VIPER_BUTTON_LEFT (1u << 8)
#define VIPER_BUTTON_RIGHT (1u << 9)
#define VIPER_BUTTON_L (1u << 10)
#define VIPER_BUTTON_R (1u << 11)

/* What loop() gets each frame: the input, and the time.
 *
 * `size` is sizeof(viper_input_t) in the firmware that filled it in. Fields
 * are only ever added at the end, so an app built with a newer viper_app.h
 * checks VIPER_INPUT_HAS() before reading a field this one doesn't list, and
 * gets 0 from older firmware. */
typedef struct {
    uint32_t size;
    uint32_t buttons;   /* VIPER_BUTTON_* bits that are down */
    int16_t touch_x;    /* where the touchscreen is touched, in screen */
    int16_t touch_y;    /* pixels; the last point while touch_down is 0 */
    uint8_t touch_down; /* 1 while touched; always 0 without a touchscreen */
    uint8_t reserved_[3];
    uint32_t now_ms; /* ms since the device started (wraps after 49 days) */
    uint32_t dt_ms;  /* ms since the last loop(): 0 for the first after
                      * create() or resume(), so time spent paused or
                      * loading doesn't count */
} viper_input_t;

#define VIPER_INPUT_HAS(input, field)                                                              \
    ((input)->size >= offsetof(viper_input_t, field) + sizeof((input)->field))

/* Just pressed: down now, up in the previous frame */
static inline int viper_button_pressed(const viper_input_t *now, const viper_input_t *previous,
                                       uint32_t button) {
    return (now->buttons & button) && !(previous->buttons & button);
}

/* Just touched: touching now, not in the previous frame */
static inline int viper_touch_began(const viper_input_t *now, const viper_input_t *previous) {
    return now->touch_down && !previous->touch_down;
}

typedef struct {
    uint32_t magic;         /* VIPER_APP_MAGIC */
    uint32_t abi_version;   /* VIPER_APP_ABI_VERSION the app was built with */
    uint32_t render_engine; /* viper_render_engine_t */

    /* Every callback may be NULL. */

    /* Build the UI. container is the app's lv_obj_t; put LVGL objects under it,
     * and the firmware deletes them when the app closes. */
    void (*create)(void *container);
    void (*resume)(void);
    void (*pause)(void);
    /* Free anything that isn't under the container. The app is unloaded right
     * after, and the loader frees heap memory it didn't. */
    void (*destroy)(void);
    /* Once a frame, with this frame's input and the last one's (zero but for
     * its size before the first frame). Returns how many ms until the next call (16
     * for 60fps): a request, so time the game by now->dt_ms.
     * The B button and the back gesture close the app, as for any app,
     * unless it called viper_app_handle_back(1). */
    uint32_t (*loop)(const viper_input_t *now, const viper_input_t *previous);
} viper_app_t;

/* The app's own folder, with its info.json, app.elf and whatever was
 * installed with them: for fopen(), e.g. "/sdcard/apps/app.viper.jokes", and
 * the same folder as LVGL names it, e.g. "Z:/apps/app.viper.jokes", for
 * lv_image_set_src(). Valid from create() until destroy() returns. */
const char *viper_app_path(void);
const char *viper_app_lvgl_path(void);

/* Asks to close the app. It closes once the callback that asked returns
 * (before the next loop()), the same as pressing B, and destroy() runs as
 * usual. Nothing in the app runs after that. */
void viper_app_exit(void);

/* 1 sends the B button and the back gesture to the app, so it can use B for
 * something else and close itself with viper_app_exit(); 0 (the default)
 * lets them close the app. It lasts until changed, or the app closes.
 * Holding Select still closes any app. */
void viper_app_handle_back(int handle);

/* Closes this app and opens the installed or built-in app `bundle_id` (e.g.
 * "app.viper.invaders") in its place, once the callback that asked returns.
 * When that app closes, this one opens again, with the `argc` strings in
 * `argv` as its arguments, instead of the home screen: a launcher opens a game
 * this way and gets it back. Returns false, and does nothing, when there's no
 * app `bundle_id`. */
bool viper_app_launch(const char *bundle_id, const char *const *argv, int argc);

/* The arguments the app was opened with: those an app it launched with
 * viper_app_launch() handed back, or none (argc 0) from the home screen.
 * viper_app_arg() is NULL for an `i` outside 0 to argc - 1. Valid from
 * create() until destroy() returns. */
int viper_app_argc(void);
const char *viper_app_arg(int i);

/* 32 random bits from the hardware random number generator. Seed a game's
 * own generator with it (a xorshift32 is a few lines) rather than calling it
 * in a loop; a seeded generator also replays the same game. Prefer it to
 * lv_rand(). */
uint32_t viper_random(void);

/* Haptics: plays one of the vibration motor's effects (a DRV2605 library
 * effect, 1-123; the common ones are below), at the strength set in Settings.
 * Does nothing when haptics are off in Settings, on a board without a motor,
 * or for an effect outside 1-123. Returns at once; a new effect replaces one
 * still playing. */
#define VIPER_HAPTIC_STRONG_CLICK 1
#define VIPER_HAPTIC_SHARP_CLICK 4
#define VIPER_HAPTIC_SOFT_BUMP 7
#define VIPER_HAPTIC_DOUBLE_CLICK 10
#define VIPER_HAPTIC_TRIPLE_CLICK 12
#define VIPER_HAPTIC_SOFT_FUZZ 13
#define VIPER_HAPTIC_STRONG_BUZZ 14
#define VIPER_HAPTIC_ALERT_750MS 15
#define VIPER_HAPTIC_ALERT_1000MS 16
#define VIPER_HAPTIC_BUZZ1 47
#define VIPER_HAPTIC_BUZZ2 48
#define VIPER_HAPTIC_BUZZ3 49
#define VIPER_HAPTIC_BUZZ4 50
#define VIPER_HAPTIC_BUZZ5 51
#define VIPER_HAPTIC_PULSING_STRONG_1 52
#define VIPER_HAPTIC_PULSING_STRONG_2 53
#define VIPER_HAPTIC_TRANSITION_CLICK_1 58
#define VIPER_HAPTIC_TRANSITION_HUM_1 64
void viper_haptic(uint8_t effect);

/* The IMU, on boards that have one: viper_imu_available() says so. Without
 * one the readings below are all zeros and return false: have another way to
 * play.
 *
 * viper_imu_accel() is what the accelerometer reads, in m/s^2: what the device
 * feels, so about 9.8 pointing up out of whatever is under it when it's held
 * still (xyz[2] when it lies flat, face up), and every jolt of a shake on top
 * of that. Gravity's pull across the screen is (-xyz[0], -xyz[1]).
 * viper_imu_gyro() is how fast it's turning about each axis, in degrees per
 * second; xyz[2] is turning in the screen's plane.
 * Both give the latest the firmware has sampled, so reading them more than
 * once a frame gains nothing. */
#define VIPER_IMU_G 9.80665f /* 1 g, in m/s^2 */
bool viper_imu_available(void);
bool viper_imu_accel(float xyz[3]);
bool viper_imu_gyro(float xyz[3]);

/* 1 keeps the screen from dimming and the device from sleeping while the app
 * is in front, as a button press each frame would: for an app played by
 * tilting, or one to watch. 0 (the default) lets them, after the usual time
 * without input. It lasts until changed, or the app closes. Turn it off when
 * nothing is happening: it costs battery. */
void viper_app_keep_awake(int on);

/* The device's state, for a status readout. viper_battery_level() is the
 * battery's charge, 0-100, as the system's battery icon shows it (a board
 * without a battery gauge reads a fixed 50). viper_battery_charging() is true
 * while it's charging. viper_memory() fills in the free and total bytes of a
 * heap, VIPER_MEMORY_INTERNAL (the chip's own RAM) or VIPER_MEMORY_PSRAM, and
 * returns false (and zeros) for one the device doesn't have. */
#define VIPER_MEMORY_INTERNAL 0u
#define VIPER_MEMORY_PSRAM 1u
int viper_battery_level(void);
bool viper_battery_charging(void);
bool viper_memory(uint32_t heap, uint32_t *free_bytes, uint32_t *total_bytes);

/* The firmware's VIPER_API_LEVEL. Itself new in API level 2, so an app that
 * calls it needs that firmware or newer. */
int viper_api_level(void);

/* A big block of memory to borrow: the PSRAM the firmware keeps for its
 * built-in emulators' ROMs, which nothing else uses while an ELF app is in
 * front. On a device with 8 MB of PSRAM it's most of what's free (2 MB), so
 * an app that needs more than the heap has left (a game engine's heap, say)
 * asks for it. Returns the block, and its size in *size, or NULL and 0 when
 * the firmware has none to lend. It holds whatever was in it before. The app
 * has it until it closes: don't free it, and don't keep it past destroy().
 * Calling it again returns the same block. */
void *viper_memory_scratch(size_t *size);

/* Sound: an audio tap, a callback the audio task calls for each block of
 * output, `frames` stereo frames of 16-bit samples (samples[2 * i] is left,
 * samples[2 * i + 1] right) at viper_audio_sample_rate(). The block arrives
 * holding other sound; add into it, clamped to int16, rather than overwrite.
 * The callback runs on the audio task, alongside loop(): keep it short, don't
 * allocate or call viper_* functions in it, and pass it work through
 * variables that one side writes and the other reads.
 * viper_audio_tap_install() returns true, or false on a board without sound, and
 * replaces a tap installed before. An app has one tap. Remove it in
 * destroy(); closing the app removes one left installed. After
 * viper_audio_tap_remove() returns, the callback isn't running and won't run
 * again. */
typedef void (*viper_audio_tap_fn)(int16_t *samples, uint32_t frames, void *user);
bool viper_audio_tap_install(viper_audio_tap_fn fn, void *user);
void viper_audio_tap_remove(void);
uint32_t viper_audio_sample_rate(void);

/* Sound for emulators and other apps that make it a frame at a time, at a
 * rate of their own: the firmware queues it and plays it at the output's
 * rate, and paces it so the speaker never runs dry. At full speed it's
 * played as made, resampled by a hair; when the app runs slow (frames come
 * late, or unevenly with frame skip), it's time-stretched, keeping its pitch,
 * so music plays slower instead of in bursts with silence between. It's the
 * firmware's built-in emulators' pacing (emu_audio_stretch.hpp).
 *
 * viper_audio_stream_open(rate, channels): `rate` samples a second
 * (4000-96000), 1 (mono) or 2 (stereo, left first) channels. False for others
 * or on a board without sound. Opening again changes the rate (a PAL game
 * switching, say) and starts the queue afresh.
 * viper_audio_stream_frame(samples, frames): one frame of the app's sound,
 * `frames` frames of `channels` 16-bit samples, as it was made. Call it once
 * an emulated frame, skipped ones included; the firmware times them itself.
 * viper_audio_stream_stretch() is how much it's stretching (1: not at all),
 * for a status line. viper_audio_stream_close() stops it; closing the app
 * does too. The stream plays beside an audio tap, which an app can still
 * have. New in API level 5. */
bool viper_audio_stream_open(uint32_t rate, uint32_t channels);
void viper_audio_stream_frame(const int16_t *samples, uint32_t frames);
float viper_audio_stream_stretch(void);
void viper_audio_stream_close(void);

/* A thread for work that runs beside loop(), such as an emulator's sound
 * chips on the other core: pthread_create(), with a name (up to 15
 * characters, shown in the diagnostic harness's task list and profiles), the
 * core it runs on (0 or 1, or -1 for either) and a priority:
 * VIPER_THREAD_PRIORITY_LOW for background work, above only the idle tasks
 * (what pthread_create() gives), or VIPER_THREAD_PRIORITY_HIGH for work a
 * frame waits on: on core 1 it shares the sound output task's priority and
 * runs ahead of the display's video task, so don't spin there for long (sleep
 * when there's nothing to do). loop() runs on core 0, so a thread on core 1
 * runs beside it.
 * `stack_size` is in bytes (at least 2048). Returns 0, or an errno value as
 * pthread_create() does. Join it with pthread_join() before destroy() returns.
 * In the host simulator it's a plain thread. New in API level 6. */
#define VIPER_THREAD_PRIORITY_LOW 0
#define VIPER_THREAD_PRIORITY_HIGH 1
int viper_thread_create(pthread_t *thread, const char *name, int core, int priority,
                        size_t stack_size, void *(*fn)(void *), void *arg);

/* Settings the app keeps between launches: options, high scores, small state
 * (docs/design/app_storage.md). They're kept in the app_nvs partition under
 * the app's bundle id, so they survive reinstalling or updating the app. Keys
 * are 1-15 characters and can't start with '_'; a string or blob is at most
 * 4KB and an app gets 8KB in all. Each set writes to flash at once, so set a
 * value when it changes (the end of a game), never every frame. A key holds
 * one type; reading it as another is the same as missing.
 *
 * viper_prefs_available() is false on a device whose partition table
 * predates app settings; then gets report missing and sets fail. */
bool viper_prefs_available(void);
bool viper_prefs_get_int(const char *key, int32_t *value); /* false if missing */
bool viper_prefs_set_int(const char *key, int32_t value);
bool viper_prefs_get_float(const char *key, float *value);
bool viper_prefs_set_float(const char *key, float value);
/* Copies the string, NUL-terminated and cut to fit `size`, like snprintf, and
 * returns its full length, or -1 if it's missing. */
int viper_prefs_get_string(const char *key, char *buf, size_t size);
bool viper_prefs_set_string(const char *key, const char *value);
/* Copies up to `size` bytes and returns the blob's full size, or -1 if it's
 * missing. Pass size 0 to ask for the size. */
int viper_prefs_get_blob(const char *key, void *buf, size_t size);
bool viper_prefs_set_blob(const char *key, const void *data, size_t size);
bool viper_prefs_remove(const char *key);

/* Save files, for anything bigger than a few KB or that users may want to copy
 * over USB: whole files in the app's data folder, data/<bundle id>/ on fatfs
 * or the SD card. `name` is a plain file name (no '/', no leading '.', not
 * ending in .new or .bak). viper_data_write() writes the whole file so that a
 * power cut leaves the old file or the new one, never a truncated one.
 *
 * viper_data_path() is the folder, for fopen() when a file is too big to hold
 * at once (then the app handles power loss itself), or NULL when the device
 * has no fatfs or SD card; then reads and writes fail. */
const char *viper_data_path(void);
/* Copies up to `size` bytes and returns the file's full size, or -1 if it's
 * missing. Pass size 0 to ask for the size. */
long viper_data_read(const char *name, void *buf, size_t size);
bool viper_data_write(const char *name, const void *data, size_t size);
bool viper_data_remove(const char *name);

/* Microseconds since the device started, for measuring how long something
 * takes. Time a game by loop()'s now_ms and dt_ms instead. Prefer either to
 * lv_tick_get(). */
uint64_t viper_time_us(void);

/* The wall clock, for a game that keeps time while it's closed: the local date
 * and time as seconds since 1970-01-01 00:00, counted as if local time were
 * UTC (so *local_s % 86400 is the time of day). Returns true when the clock
 * has been set, false when it hasn't: a device's clock starts near 1970 at
 * every boot until something sets it. *local_s holds what the clock reads
 * either way. */
bool viper_time_local(int64_t *local_s);

/* Writes a line to the log (the serial console), printf-style. A newline at
 * the end is optional. */
void viper_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Drawing, for a VIPER_RENDER_VIDEO_BLIT app.
 *
 * The screen is viper_display_width() x viper_display_height() RGB565 pixels
 * (red in the top 5 bits; see VIPER_RGB565()), row by row with no padding.
 * Draw a frame in loop():
 *     uint16_t *pixels = viper_frame_begin();
 *     ... write pixels[y * width + x] ...
 *     viper_frame_mark_dirty(x, y, w, h);   for each area that changed, or
 *     viper_frame_end(1);                   to send the whole screen
 * The buffer keeps the frame last drawn, so an app only redraws what
 * changed, and only marked areas reach the display unless flush_all is 1.
 * The pointer can change from frame to frame: get it again each frame, and
 * don't keep it past viper_frame_end(). In an LVGL app viper_frame_begin()
 * returns NULL and the others do nothing. */
uint32_t viper_display_width(void);
uint32_t viper_display_height(void);
uint16_t *viper_frame_begin(void);
void viper_frame_mark_dirty(int x, int y, int width, int height);
void viper_frame_end(int flush_all);

/* Indexed frames, for emulators and other apps that draw with a palette: the
 * display stretches a smaller picture to the screen, as scan-out does for the
 * built-in emulators, so the app writes a byte a pixel and nothing scales.
 *
 * After viper_frame_set_indexed(width, height, palette), viper_frame_begin()'s
 * buffer holds width x height bytes (cast it to uint8_t *), row by row with no
 * padding, each an index into `palette`: 256 RGB565 colors, which are copied,
 * so call it again after changing them. Send each frame whole with
 * viper_frame_end(1); viper_frame_mark_dirty() doesn't apply. Draw it all
 * after switching, since what the buffer held isn't kept.
 * viper_frame_set_indexed(0, 0, NULL) goes back to RGB565 at the screen's
 * size (for a pause menu, say); closing the app does that too.
 * Returns false, and changes nothing, in an LVGL app or for a size of 0 or
 * larger than the screen. */
bool viper_frame_set_indexed(uint32_t width, uint32_t height, const uint16_t *palette);

/* For indexed frames that are drawn whole each time (every pixel, every
 * frame), as an emulator's are: viper_frame_set_swap(1) has two buffers take
 * turns, the app drawing one while the display sends the other, instead of
 * viper_frame_end() copying each frame for the display (70 KB at 320x224,
 * about 4 ms on an S3). viper_frame_begin()'s buffer then holds the frame
 * before last, not the last, so draw all of it. Call it after
 * viper_frame_set_indexed(); going back to RGB565
 * (viper_frame_set_indexed(0, 0, NULL)) turns it off. False, and nothing
 * changes, outside an indexed frame. New in API level 6. */
bool viper_frame_set_swap(int on);

/* The system keyboard, for a line of text: a name, a save's title, a wish.
 * New in API level 4.
 *
 * viper_text_entry_open() opens it over the app and returns at once; the app
 * keeps getting loop() calls and polls viper_text_entry_poll() in them. While
 * it's open the buttons go to the keyboard (the app sees none down, and those
 * held when it closes stay hidden until they're let go), and a VIDEO_BLIT
 * app's frames aren't shown: viper_frame_begin() returns NULL, and the
 * keyboard sits over a still of the app's last frame. Closing the app closes
 * the keyboard. */
typedef struct {
    uint32_t size;       /* sizeof(viper_text_entry_t) */
    const char *title;   /* what's asked, or NULL */
    const char *text;    /* the text it starts with, or NULL */
    uint32_t max_length; /* in characters; 0 for no limit */
} viper_text_entry_t;

#define VIPER_TEXT_ENTRY_NONE 0      /* none is open, or its answer has been read */
#define VIPER_TEXT_ENTRY_OPEN 1      /* still open */
#define VIPER_TEXT_ENTRY_DONE 2      /* the player entered text: it's in buf */
#define VIPER_TEXT_ENTRY_CANCELLED 3 /* the player backed out */

/* Opens the keyboard. Returns false, and opens nothing, if one is open
 * already, outside create()..destroy(), or for a bad config. */
bool viper_text_entry_open(const viper_text_entry_t *config);

/* How the keyboard viper_text_entry_open() opened is going. When it closes,
 * it returns VIPER_TEXT_ENTRY_DONE (with the text, UTF-8, copied to buf: cut
 * to fit `size` bytes, always NUL-terminated) or VIPER_TEXT_ENTRY_CANCELLED,
 * once; after that, VIPER_TEXT_ENTRY_NONE. */
int viper_text_entry_poll(char *buf, size_t size);

/* A pixel from 8-bit red, green and blue */
#define VIPER_RGB565(r, g, b)                                                                      \
    ((uint16_t)((((r)&0xF8) << 8) | (((g)&0xFC) << 3) | (((b)&0xFF) >> 3)))

/* Makes app_main() return the app's descriptor. Use it once, in one file.
 *
 * The host simulator compiles apps into one program instead of loading them,
 * so there each app's function gets its own name, from VIPER_APP_STATIC_NAME
 * (the app's folder name; set by sim/CMakeLists.txt). Code in an app is the
 * same either way, except that its globals keep their values between
 * launches in the simulator, while a device loads the app afresh each time:
 * set them in create(). */
#ifdef __cplusplus
#define VIPER_APP_EXTERN_C extern "C"
#else
#define VIPER_APP_EXTERN_C
#endif

#ifdef VIPER_APP_STATIC_NAME
#define VIPER_APP_CONCAT_(a, b) a##b
#define VIPER_APP_CONCAT(a, b) VIPER_APP_CONCAT_(a, b)
#define VIPER_APP(descriptor)                                                                      \
    VIPER_APP_EXTERN_C const viper_app_t *VIPER_APP_CONCAT(viper_app_,                             \
                                                           VIPER_APP_STATIC_NAME)(void) {          \
        return &(descriptor);                                                                      \
    }
#elif defined(__cplusplus)
/* C++ apps also define __dso_handle, which the C runtime's startup files
 * normally provide and apps don't link: the compiler passes it to
 * __cxa_atexit() when a global with a destructor is constructed, and the
 * firmware runs those destructors when the app is unloaded. GCC declares
 * __dso_handle itself, so it's defined under another name. */
#define VIPER_APP(descriptor)                                                                      \
    __attribute__((visibility("hidden"))) void *viper_app_dso_handle_ __asm__("__dso_handle") =    \
        &viper_app_dso_handle_;                                                                    \
    extern "C" const viper_app_t *app_main(void) { return &(descriptor); }
#else
#define VIPER_APP(descriptor)                                                                      \
    const viper_app_t *app_main(void) { return &(descriptor); }
#endif

#ifdef __cplusplus
}
#endif
