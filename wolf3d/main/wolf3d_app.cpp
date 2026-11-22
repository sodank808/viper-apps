// Wolfenstein 3D (Wolf4SDL, the portable port of id Software's source) as a
// Viper ELF app.
//
// The engine is written as the DOS original was: one long-running program
// that loops until something happens (a key in a menu, the end of a fade,
// the next tic). So it runs on a thread of its own, and wherever it would
// wait (showing a frame, a delay, waiting for a key) it hands the turn to
// loop() and waits for the next frame (yield_frame()). Only one side runs at
// a time, so the engine needs no locks: loop() gives the engine this
// frame's buttons and time, the engine runs until it waits, and loop() shows
// what it drew.
//
// The picture is the engine's 320x200 8-bit screen, handed to the display as
// an indexed frame with the game's palette, which stretches it to the screen.
// Sound is mixed on the engine's thread, a frame ahead, into a ring the
// audio tap reads (wolf3d_sound.cpp).

#include <pthread.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gfx/font_unscii8.hpp"
#include "gfx/surface.hpp"
#include "gfx/ui.hpp"
#include "viper_app.h"
#include "wolf3d_app.h"

// The engine's keys (SDL's scan codes). The app's code doesn't include
// engine/wolf_names.h: it would rename the app's own names too.
#include "engine/wolf_sdl.h"

extern "C" void wolf3d_main(void);
// The engine's Quit() (by its prefixed name, engine/wolf_names.h): saves the
// config, shuts the engine down, and leaves (wolf3d_quit())
extern "C" void wolf_Quit(const char *error, ...);

// The simulator links the app into its own program, so the engine's
// variables keep their values from one launch to the next (a device loads
// the app afresh). The engine's are in sections of their own there
// (engine/wl_def.h), which these bound.
#if defined(VIPER_APP_STATIC_NAME) && defined(__clang__)
#define WOLF3D_RESTART_STATE 1
#ifdef __APPLE__
extern char wolf_data_start __asm("section$start$__DATA$__wolf_data");
extern char wolf_data_end __asm("section$end$__DATA$__wolf_data");
extern char wolf_bss_start __asm("section$start$__DATA$__wolf_bss");
extern char wolf_bss_end __asm("section$end$__DATA$__wolf_bss");
#else
extern "C" char __start_wolf_data, __stop_wolf_data, __start_wolf_bss, __stop_wolf_bss;
#define wolf_data_start __start_wolf_data
#define wolf_data_end __stop_wolf_data
#define wolf_bss_start __start_wolf_bss
#define wolf_bss_end __stop_wolf_bss
#endif
#endif

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 200;
// The engine thread's stack. The simulator's sanitizers make frames many
// times bigger
#ifdef VIPER_APP_STATIC_NAME
constexpr size_t kEngineStack = 1024 * 1024;
#else
constexpr size_t kEngineStack = 32 * 1024;
#endif
// How long loop() waits for the engine to finish a frame before it returns
// and waits again in the next one (a level loading, say). The simulator waits
// longer, so the tests' frames don't run ahead of a slow sanitized engine.
#ifdef VIPER_APP_STATIC_NAME
constexpr long kFrameWaitMs = 5000;
#else
constexpr long kFrameWaitMs = 100;
#endif

// The data files the engine opens, which must all be there
// Two presses this close together are a double press: Up twice (held) runs,
// B twice is the next weapon, for boards without X and Y. A single B waits
// this long to open a door, in case a second one follows.
constexpr uint32_t kDoublePressMs = 250;

constexpr const char *kDataFiles[] = {"vswap.wl1",   "maphead.wl1",  "gamemaps.wl1", "vgahead.wl1",
                                      "vgadict.wl1", "vgagraph.wl1", "audiohed.wl1", "audiot.wl1"};

enum class State {
    Running, // the engine is running
    Message, // showing message (missing data, an error), waiting for a button
};

// A block the engine allocated: kept in a list, so that closing the app
// frees them all, wherever the engine was. Four words (16 bytes on a
// device), so what follows is as aligned as malloc()'s
struct Block {
    Block *prev;
    Block *next;
    size_t size;
    size_t unused;
};

// A key event for the engine's input layer
struct KeyEvent {
    int scancode;
    bool down;
};

struct Wolf {
    State state = State::Running;
    std::string data_dir; // the app's folder, with the *.wl1 files
    std::string save_dir; // the data folder, or empty
    std::string message;
    bool message_drawn = false;

    // The engine thread, and the turn between it and loop()
    pthread_t thread{};
    bool thread_started = false;
    pthread_mutex_t mutex{}; // made in create(), destroyed in destroy()
    pthread_cond_t cond{};
    bool engine_turn = true; // the engine is running (it starts so)
    bool stop = false;       // destroy() asks the engine to stop
    bool stopping = false;   // the engine is quitting because of it
    bool finished = false;   // the engine thread has returned
    jmp_buf engine_exit;     // where wolf3d_quit() goes
    std::string error;       // what the engine quit with

    // Handed from loop() to the engine, under the mutex
    uint32_t in_buttons = 0;
    uint32_t in_now_ms = 0;

    // The engine's, while it runs
    uint32_t start_ms = 0;
    uint32_t ticks = 0;
    uint32_t buttons = 0; // as of the last frame the engine saw
    bool playing = false;
    bool menu = false;
    // What each button's press sent, to release the same keys
    int pressed[16][2] = {};
    std::vector<KeyEvent> keys;
    // The gestures for boards without X and Y (kDoublePressMs): when Up was
    // last let go, whether a press of B waits to see if another follows, and
    // the keys a tap sent, let go of in the next frame
    uint32_t up_released = 0;
    bool up_was_released = false;
    bool b_waiting = false;
    uint32_t b_pressed = 0;
    std::vector<int> tapped;
    size_t key_read = 0;

    // From the engine to loop(), while the engine waits
    const uint8_t *frame = nullptr; // shown at the next loop()
    uint32_t frames = 0;            // shown
    uint16_t palette[256] = {};
    bool palette_dirty = true;

    Block blocks{}; // the list of the engine's memory
    size_t heap_used = 0;
    size_t heap_peak = 0;
};

Wolf *g = nullptr;

// ---- messages (missing data, errors) ------------------------------------------

void draw_message(const std::string &text) {
    viper_frame_set_indexed(0, 0, nullptr);
    const int w = static_cast<int>(viper_display_width());
    const int h = static_cast<int>(viper_display_height());
    uint16_t *px = viper_frame_begin();
    if (!px) {
        return;
    }
    gfx::Surface s(px, w, h);
    s.fill(s.bounds(), 0);
    gfx::ui::Theme t = gfx::ui::kSystemTheme;
    t.font = &gfx::kFontUnscii8; // lower case, and the paths' punctuation
    t.scale = w >= 300 ? 2 : 1;
    std::vector<std::string> lines(1);
    for (char c : text) {
        if (c == '\n') {
            lines.emplace_back();
        } else {
            lines.back() += c;
        }
    }
    while (t.scale > 1 && std::any_of(lines.begin(), lines.end(), [&](const std::string &l) {
               return gfx::text_width(*t.font, l, t.scale) > w - 16;
           })) {
        t.scale--;
    }
    const int pitch = gfx::ui::Menu::pitch(t, t.scale);
    int y = (h - pitch * static_cast<int>(lines.size())) / 2;
    for (size_t i = 0; i < lines.size(); i++) {
        gfx::ui::text_centered(s, t, lines[i], 0, w, y, i == 0 ? t.accent : t.text, t.scale);
        y += pitch;
    }
    viper_frame_end(1);
}

void show_message(const std::string &text) {
    g->message = text;
    g->message_drawn = false;
    g->state = State::Message;
}

// ---- files ------------------------------------------------------------------------

bool file_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// `name` in `dir`, as it is, in lower case or in upper case (as the DOS
// release names them): the path that's there, or empty
std::string find_file(const std::string &dir, const char *name) {
    std::string lower(name), upper(name);
    // (by hand: the firmware doesn't export tolower() and toupper())
    for (auto &c : lower) {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    for (auto &c : upper) {
        c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }
    for (const std::string &n : {std::string(name), lower, upper}) {
        std::string path = dir + "/" + n;
        if (file_exists(path)) {
            return path;
        }
    }
    return {};
}

// Makes `path` and the folders it's in. (A loop over the characters:
// std::string::find() calls memchr(), which the firmware doesn't export.)
void make_dirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            mkdir(path.substr(0, i).c_str(), 0755);
        }
    }
}

// The data files that aren't in `dir`, one a line
std::string missing_data(const std::string &dir) {
    std::string missing;
    for (const char *name : kDataFiles) {
        if (find_file(dir, name).empty()) {
            missing += std::string(name) + "\n";
        }
    }
    return missing;
}

// ---- starting the engine again (the simulator) ----------------------------------

#ifdef WOLF3D_RESTART_STATE
// The engine's initialized variables as the program loaded them. Static, so
// the tests that check an app's launches don't count it as the app's.
char g_initial_data[256 * 1024];
bool g_saved = false;

// Byte by byte, unchecked: the sections hold AddressSanitizer's red zones
// around each variable, which a checked copy would stop at
__attribute__((no_sanitize("address"))) void copy_bytes(volatile char *to,
                                                        const volatile char *from, size_t n) {
    for (size_t i = 0; i < n; i++) {
        to[i] = from[i];
    }
}
#endif

// Puts the engine's variables back as they were before it first ran in this
// program: on a device nothing to do. Returns false where that can't be done
// (a simulator built with GCC) and the engine has run before.
bool restart_state() {
#ifdef WOLF3D_RESTART_STATE
    const size_t data = &wolf_data_end - &wolf_data_start;
    const size_t bss = &wolf_bss_end - &wolf_bss_start;
    if (data > sizeof(g_initial_data)) {
        viper_log("wolf3d: the engine's data (%u KB) doesn't fit the copy", unsigned(data / 1024));
        return !g_saved;
    }
    if (!g_saved) {
        copy_bytes(g_initial_data, &wolf_data_start, data);
        g_saved = true;
        return true;
    }
    copy_bytes(&wolf_data_start, g_initial_data, data);
    std::vector<char> zeros(bss); // (copied too, for the same reason)
    copy_bytes(&wolf_bss_start, zeros.data(), bss);
    return true;
#elif defined(VIPER_APP_STATIC_NAME)
    static bool ran = false;
    const bool first = !ran;
    ran = true;
    return first;
#else
    return true;
#endif
}

// ---- the turn between loop() and the engine ------------------------------------

// The engine thread: waits for its turn, then takes this frame's input.
// When destroy() asks it to stop, it quits as the game's own Quit does
// (saving the config, shutting down), back to engine_main().
void yield_frame() {
    if (g->stopping) {
        return; // quitting: nothing waits for a frame now
    }
    wolf3d_sound_fill();
    pthread_mutex_lock(&g->mutex);
    g->engine_turn = false;
    pthread_cond_broadcast(&g->cond);
    while (!g->engine_turn && !g->stop) {
        pthread_cond_wait(&g->cond, &g->mutex);
    }
    const bool stop = g->stop;
    const uint32_t buttons = g->in_buttons;
    g->ticks = g->in_now_ms - g->start_ms;
    pthread_mutex_unlock(&g->mutex);
    if (stop) {
        g->stopping = true;
        wolf_Quit(nullptr);
    }

    // Each button's press and release, as keys: what a button is depends on
    // whether the game is playing or in a menu when it's pressed, and its
    // release lets go of the keys its press sent
    struct Map {
        uint32_t button;
        int play[2];
        int menu[2];
    };
    static const Map kMap[] = {
        {VIPER_BUTTON_UP, {SDL_SCANCODE_UP, 0}, {SDL_SCANCODE_UP, 0}},
        {VIPER_BUTTON_DOWN, {SDL_SCANCODE_DOWN, 0}, {SDL_SCANCODE_DOWN, 0}},
        {VIPER_BUTTON_LEFT, {SDL_SCANCODE_LEFT, 0}, {SDL_SCANCODE_LEFT, 0}},
        {VIPER_BUTTON_RIGHT, {SDL_SCANCODE_RIGHT, 0}, {SDL_SCANCODE_RIGHT, 0}},
        // Fire; in a menu choose, and answer yes
        {VIPER_BUTTON_A, {SDL_SCANCODE_LCTRL, 0}, {SDL_SCANCODE_RETURN, SDL_SCANCODE_Y}},
        // Open doors (sent by the double press below); in a menu go back,
        // and answer no
        {VIPER_BUTTON_B, {0, 0}, {SDL_SCANCODE_ESCAPE, 0}},
        {VIPER_BUTTON_X, {SDL_SCANCODE_LSHIFT, 0}, {0, 0}},                       // run
        {VIPER_BUTTON_Y, {SDL_SCANCODE_PERIOD, 0}, {0, 0}},                       // next weapon
        {VIPER_BUTTON_L, {SDL_SCANCODE_Q, 0}, {0, 0}},                            // strafe left
        {VIPER_BUTTON_R, {SDL_SCANCODE_E, 0}, {0, 0}},                            // strafe right
        {VIPER_BUTTON_START, {SDL_SCANCODE_ESCAPE, 0}, {SDL_SCANCODE_ESCAPE, 0}}, // menu
    };
    if (g->key_read >= g->keys.size()) {
        g->keys.clear();
        g->key_read = 0;
    }
    // Taps last a frame
    for (int key : g->tapped) {
        g->keys.push_back({key, false});
    }
    g->tapped.clear();
    auto tap = [](int key) {
        g->keys.push_back({key, true});
        g->tapped.push_back(key);
    };
    const uint32_t now_ms = g->ticks;
    const bool b_down = buttons & VIPER_BUTTON_B;
    const bool b_before = g->buttons & VIPER_BUTTON_B;

    // B, playing: a second press soon after the first is the next weapon;
    // otherwise, once that's too late, the first opens a door (held as long
    // as B is)
    int *b_keys = nullptr; // what B's release lets go of (set in the loop)
    if (!g->playing) {
        g->b_waiting = false;
    } else if (b_down && !b_before && g->b_waiting) {
        g->b_waiting = false;
        tap(SDL_SCANCODE_PERIOD);
    } else if (b_down && !b_before) {
        g->b_waiting = true;
        g->b_pressed = now_ms;
    }

    int i = 0;
    for (const Map &m : kMap) {
        const bool now = buttons & m.button;
        const bool before = g->buttons & m.button;
        if (m.button == VIPER_BUTTON_B) {
            b_keys = g->pressed[i];
        }
        if (now && !before) {
            const int *keys = g->playing ? m.play : m.menu;
            for (int k = 0; k < 2; k++) {
                g->pressed[i][k] = keys[k];
                if (keys[k]) {
                    g->keys.push_back({keys[k], true});
                }
            }
            // Up again soon after it was let go, playing: run while it's held
            if (m.button == VIPER_BUTTON_UP && g->playing && g->up_was_released &&
                now_ms - g->up_released <= kDoublePressMs) {
                g->pressed[i][1] = SDL_SCANCODE_LSHIFT;
                g->keys.push_back({SDL_SCANCODE_LSHIFT, true});
            }
        } else if (!now && before) {
            if (m.button == VIPER_BUTTON_UP) {
                g->up_released = now_ms;
                g->up_was_released = true;
            }
            for (int k = 0; k < 2; k++) {
                if (g->pressed[i][k]) {
                    g->keys.push_back({g->pressed[i][k], false});
                    g->pressed[i][k] = 0;
                }
            }
        }
        i++;
    }

    // A single B, once a second can't follow: the door
    if (g->b_waiting && now_ms - g->b_pressed > kDoublePressMs) {
        g->b_waiting = false;
        if (b_down) {
            b_keys[0] = SDL_SCANCODE_SPACE;
            g->keys.push_back({SDL_SCANCODE_SPACE, true});
        } else {
            tap(SDL_SCANCODE_SPACE);
        }
    }
    g->buttons = buttons;
}

void *engine_main(void *) {
    if (setjmp(g->engine_exit) == 0) {
        wolf3d_main();
    }
    pthread_mutex_lock(&g->mutex);
    g->finished = true;
    g->engine_turn = false;
    pthread_cond_broadcast(&g->cond);
    pthread_mutex_unlock(&g->mutex);
    return nullptr;
}

// Stops the engine, wherever it is, and frees what it allocated
void stop_engine() {
    if (g->thread_started) {
        pthread_mutex_lock(&g->mutex);
        g->stop = true;
        pthread_cond_broadcast(&g->cond);
        pthread_mutex_unlock(&g->mutex);
        pthread_join(g->thread, nullptr);
        g->thread_started = false;
    }
    wolf3d_sound_stop();
    Block *b = g->blocks.next;
    while (b && b != &g->blocks) {
        Block *next = b->next;
        std::free(b);
        b = next;
    }
    g->blocks.next = g->blocks.prev = &g->blocks;
    g->heap_used = 0;
}

void show_frame() {
    if (g->palette_dirty) {
        viper_frame_set_indexed(kWidth, kHeight, g->palette);
        g->palette_dirty = false;
    }
    auto *px = reinterpret_cast<uint8_t *>(viper_frame_begin());
    if (px) {
        std::memcpy(px, g->frame, kWidth * kHeight);
        viper_frame_end(1);
    }
    g->frame = nullptr;
    g->frames++;
}

// ---- the app --------------------------------------------------------------------

void create(void *) {
    g = new Wolf;
    g->blocks.next = g->blocks.prev = &g->blocks;
    pthread_mutex_init(&g->mutex, nullptr);
    pthread_cond_init(&g->cond, nullptr);
    g->data_dir = viper_app_path() ? viper_app_path() : ".";
    if (viper_data_path()) {
        g->save_dir = viper_data_path();
        make_dirs(g->save_dir); // the engine makes only the last folder
    }

    const std::string missing = missing_data(g->data_dir);
    if (!missing.empty()) {
        viper_log("wolf3d: no game data in %s", g->data_dir.c_str());
        show_message("WOLFENSTEIN 3D\n\nCopy the shareware data\n(*.WL1) into the app's folder:\n" +
                     g->data_dir + "\n\nMissing:\n" + missing);
        return;
    }

    if (!restart_state()) {
        show_message("WOLFENSTEIN 3D\n\nThis simulator can run the game\nonce: restart it to play "
                     "again\n(or build it with Clang)");
        return;
    }

    // B opens doors; the game's own Quit closes the app (or holding Select)
    viper_app_handle_back(1);
    wolf3d_sound_start();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kEngineStack);
    g->thread_started = pthread_create(&g->thread, &attr, engine_main, nullptr) == 0;
    if (!g->thread_started) {
        show_message("WOLFENSTEIN 3D\n\nCouldn't start");
    }
}

void destroy() {
    if (!g) {
        return;
    }
    stop_engine();
    viper_log("wolf3d: closed; heap peak %u KB", unsigned(g->heap_peak / 1024));
    pthread_cond_destroy(&g->cond);
    pthread_mutex_destroy(&g->mutex);
    delete g;
    g = nullptr;
}

uint32_t loop(const viper_input_t *now, const viper_input_t *previous) {
    if (g->state == State::Message) {
        if (!g->message_drawn) {
            draw_message(g->message);
            g->message_drawn = true;
        }
        constexpr uint32_t kAny = VIPER_BUTTON_A | VIPER_BUTTON_B | VIPER_BUTTON_START;
        if (viper_button_pressed(now, previous, kAny)) {
            viper_app_exit();
        }
        return 30;
    }

    // The engine's turn, with this frame's buttons and time
    pthread_mutex_lock(&g->mutex);
    if (g->start_ms == 0) {
        g->start_ms = now->now_ms;
    }
    g->in_buttons = now->buttons;
    g->in_now_ms = now->now_ms;
    if (!g->engine_turn && !g->finished) {
        g->engine_turn = true;
        pthread_cond_broadcast(&g->cond);
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += kFrameWaitMs / 1000;
    deadline.tv_nsec += (kFrameWaitMs % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (g->engine_turn && !g->finished) {
        if (pthread_cond_timedwait(&g->cond, &g->mutex, &deadline) != 0) {
            break; // still busy: wait again next frame
        }
    }
    const bool waiting = !g->engine_turn || g->finished;
    const bool finished = g->finished;
    pthread_mutex_unlock(&g->mutex);

    if (finished) {
        pthread_join(g->thread, nullptr);
        g->thread_started = false;
        if (g->error.empty()) {
            viper_app_exit();
        } else {
            viper_log("wolf3d: %s", g->error.c_str());
            stop_engine();
            show_message("WOLFENSTEIN 3D\n\n" + g->error);
        }
        return 16;
    }
    if (waiting && g->frame) {
        show_frame();
    }
    return 16;
}

const viper_app_t app = {
    .magic = VIPER_APP_MAGIC,
    .abi_version = VIPER_APP_ABI_VERSION,
    .render_engine = VIPER_RENDER_VIDEO_BLIT,
    .create = create,
    .resume = nullptr,
    .pause = nullptr,
    .destroy = destroy,
    .loop = loop,
};

} // namespace

wolf3d::Status wolf3d::status() {
    Status s;
    if (!g) {
        return s;
    }
    s.open = true;
    s.running = g->state == State::Running && g->thread_started;
    if (g->state == State::Message) {
        s.message = g->message;
    }
    s.playing = g->playing;
    s.menu = g->menu;
    if (s.running && g->playing) {
        s.weapon = wolf3d_weapon();
        s.run = wolf3d_running() != 0;
    }
    s.frames = g->frames;
    s.heap_used = g->heap_used;
    s.heap_peak = g->heap_peak;
    return s;
}

// ---- the platform the engine calls (wolf3d_platform.h) --------------------------

extern "C" {

uint32_t wolf3d_ticks(void) { return g->ticks; }

void wolf3d_next_frame(void) { yield_frame(); }

void wolf3d_delay(uint32_t ms) {
    if (ms == 0) {
        return;
    }
    const uint32_t until = g->ticks + ms;
    do {
        yield_frame();
    } while (static_cast<int32_t>(until - g->ticks) > 0);
}

void wolf3d_set_palette(const uint8_t *rgba) {
    for (int i = 0; i < 256; i++) {
        g->palette[i] = VIPER_RGB565(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
    }
    g->palette_dirty = true;
}

void wolf3d_present(const uint8_t *pixels) {
    g->frame = pixels;
    yield_frame();
}

int wolf3d_poll_key(int *scancode, int *down) {
    if (g->key_read >= g->keys.size()) {
        return 0;
    }
    *scancode = g->keys[g->key_read].scancode;
    *down = g->keys[g->key_read].down ? 1 : 0;
    g->key_read++;
    return 1;
}

void wolf3d_set_playing(int playing) { g->playing = playing != 0; }

void wolf3d_set_menu(int open) { g->menu = open != 0; }

FILE *wolf3d_fopen(const char *name, const char *mode) {
    if (std::strchr(name, '/')) {
        return std::fopen(name, mode);
    }
    std::string path = find_file(g->data_dir, name);
    if (path.empty()) {
        path = g->data_dir + "/" + name;
    }
    return std::fopen(path.c_str(), mode);
}

size_t wolf3d_page_cache_size(void) {
#ifdef VIPER_APP_STATIC_NAME
    // The simulator has memory to spare; this is what a device can give, so
    // that the tests page as a device does
    return 192 * 1024;
#else
    // What's free in PSRAM, less 512 KB for the rest of the game (its
    // graphics, sounds and maps) and the system, between 128 KB and all of
    // the shareware's walls and sprites
    uint32_t free_bytes = 0, total = 0;
    viper_memory(VIPER_MEMORY_PSRAM, &free_bytes, &total);
    const size_t reserve = 512 * 1024;
    const size_t room = free_bytes > reserve ? free_bytes - reserve : 0;
    return std::min<size_t>(std::max<size_t>(room, 128 * 1024), 576 * 1024);
#endif
}

const char *wolf3d_save_dir(void) { return g->save_dir.empty() ? nullptr : g->save_dir.c_str(); }

void wolf3d_quit(const char *error) {
    g->error = error ? error : "";
    longjmp(g->engine_exit, 1);
}

void *wolf3d_malloc(size_t size) {
    auto *b = static_cast<Block *>(std::malloc(sizeof(Block) + size));
    if (!b) {
        return nullptr;
    }
    b->size = size;
    b->next = g->blocks.next;
    b->prev = &g->blocks;
    g->blocks.next->prev = b;
    g->blocks.next = b;
    g->heap_used += size;
    g->heap_peak = std::max(g->heap_peak, g->heap_used);
    return b + 1;
}

void wolf3d_free(void *ptr) {
    if (!ptr) {
        return;
    }
    Block *b = static_cast<Block *>(ptr) - 1;
    b->prev->next = b->next;
    b->next->prev = b->prev;
    g->heap_used -= b->size;
    std::free(b);
}

void *wolf3d_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        return nullptr;
    }
    void *p = wolf3d_malloc(count * size);
    if (p) {
        std::memset(p, 0, count * size);
    }
    return p;
}

char *wolf3d_strdup(const char *str) {
    const size_t n = std::strlen(str) + 1;
    auto *p = static_cast<char *>(wolf3d_malloc(n));
    if (p) {
        std::memcpy(p, str, n);
    }
    return p;
}

void *wolf3d_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return wolf3d_malloc(size);
    }
    if (size == 0) {
        wolf3d_free(ptr);
        return nullptr;
    }
    const size_t old = (static_cast<Block *>(ptr) - 1)->size;
    void *p = wolf3d_malloc(size);
    if (p) {
        std::memcpy(p, ptr, std::min(old, size));
        wolf3d_free(ptr);
    }
    return p;
}

} // extern "C"

VIPER_APP(app)
