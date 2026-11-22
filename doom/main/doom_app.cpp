// Doom (PrBoom, by way of esp-box-emu) as a Viper ELF app.
//
// The engine runs on the app's thread, a frame of it per loop(): the tics
// that are due (35 a second), then one D_Display(). Setting up the engine
// (reading the WAD's directory, building the texture tables) takes seconds,
// so it runs on a thread of its own while loop() shows that it's loading.
//
// The picture is Doom's own 320x200 8-bit screen, handed to the display as
// an indexed frame with Doom's palette, which stretches it to the screen.

#include <pthread.h>
#include <setjmp.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doom_memory.h"
#include "doom_port.h"
#include "gfx/surface.hpp"
#include "gfx/ui.hpp"
#include "viper_app.h"

extern "C" {
// The engine's headers expect these first
#include "prboom/doomdef.h"
#include "prboom/doomstat.h"
#include "prboom/doomtype.h"

#include "prboom/d_event.h"
#include "prboom/d_main.h"
#include "prboom/d_net.h"
#include "prboom/g_game.h"
#include "prboom/i_main.h"
#include "prboom/i_sound.h"
#include "prboom/i_system.h"
#include "prboom/i_video.h"
#include "prboom/m_argv.h"
#include "prboom/m_menu.h"
#include "prboom/m_misc.h"
#include "prboom/r_draw.h"
#include "prboom/r_fps.h"
#include "prboom/r_main.h"
#include "prboom/s_sound.h"
#include "prboom/st_stuff.h"
#include "prboom/v_video.h"
#include "prboom/w_wad.h"
#include "prboom/z_zone.h"

// Defined in the engine: the setup of a loop iteration (d_client.c), and
// the resets esp-box-emu added for an engine that starts again in the same
// program (as it does in the simulator, where globals keep their values)
void D_BuildNewTiccmds(void);
void R_ResetDrawSegs(void);
void R_ResetPlanes(void);
void R_ResetVisSprites(void);
void R_ResetViewMapping(void);
void R_ResetInterpolations(void);
void P_ResetSpechit(void);
void P_ResetIntercepts(void);
void P_ResetMapStarts(void);
void G_ResetBodyQueue(void);
void P_ResetBrainTargets(void);
void AM_ResetMarks(void);
void W_Done(void);
void R_ForceRedraw(void);
void doom_fatal(const char *message);
}

// z_zone.h turns malloc() and free() into the zone's; this file's own memory
// is the heap's, and what the engine allocated goes back with Z_Free()
#undef malloc
#undef free
#undef realloc
#undef calloc
#undef strdup

namespace {

constexpr int kWidth = 320;  // SCREENWIDTH
constexpr int kHeight = 200; // SCREENHEIGHT
// The setup thread's stack. The simulator's sanitizers make frames many
// times bigger
#ifdef VIPER_APP_STATIC_NAME
constexpr size_t kInitStack = 1024 * 1024;
#else
constexpr size_t kInitStack = 24 * 1024;
#endif

enum class State {
    Loading, // the init thread is setting up the engine
    Running,
    Message, // an error (or no WAD): showing message_, waiting for a button
    Quit,
};

struct Doom {
    State state = State::Loading;
    std::string wad;
    std::string save_dir;
    std::string message;
    const char *argv[8] = {};

    // The init thread
    pthread_t init_thread{};
    bool init_started = false;
    std::atomic<bool> init_done{false};
    bool init_ok = false;

    // Where doom_fatal() goes: set by whichever side is running engine code
    jmp_buf *jump = nullptr;
    char fatal[256] = {};

    uint8_t *screen = nullptr; // screens[0]
    uint16_t palette[256] = {};
    std::atomic<bool> palette_dirty{false};
    bool in_loop = false;        // I_FinishUpdate() can draw
    uint32_t buttons = 0;        // this frame's, for I_StartTic()
    uint32_t posted_buttons = 0; // what the engine has been told
    bool message_drawn = false;
    bool sound = false;
    uint32_t frames = 0;
    uint64_t engine_us = 0;
};

Doom *g = nullptr;

// ---- messages (loading, errors) -----------------------------------------------

void draw_message(const std::string &text, uint16_t color) {
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
    for (const auto &line : lines) {
        gfx::ui::text_centered(s, t, line, 0, w, y, color, t.scale);
        y += pitch;
    }
    viper_frame_end(1);
}

void show_message(const std::string &text) {
    g->message = text;
    g->message_drawn = false;
    g->state = State::Message;
}

// ---- finding the WAD --------------------------------------------------------------

bool file_exists(const std::string &path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The WAD the app was opened with (doom://<path>, from the ROM picker or
// Files), or one installed with the app
std::string find_wad() {
    if (viper_app_argc() > 0 && viper_app_arg(0) && *viper_app_arg(0)) {
        return viper_app_arg(0);
    }
    const char *dir = viper_app_path();
    if (dir) {
        for (const char *name : {"doom1.wad", "doom.wad", "doom2.wad"}) {
            std::string path = std::string(dir) + "/" + name;
            if (file_exists(path)) {
                return path;
            }
        }
    }
    return {};
}

// ---- the engine ------------------------------------------------------------------

// Sets up the engine: on the init thread
void *init_main(void *) {
    jmp_buf jump;
    g->jump = &jump;
    if (setjmp(jump) == 0) {
        doom_statics_init();
        SCREENWIDTH = kWidth;
        SCREENHEIGHT = kHeight;
        int argc = 0;
        g->argv[argc++] = "doom";
        g->argv[argc++] = "-iwad";
        g->argv[argc++] = g->wad.c_str();
        if (!g->save_dir.empty()) {
            g->argv[argc++] = "-save";
            g->argv[argc++] = g->save_dir.c_str();
        }
        g->argv[argc] = nullptr;
        myargv = g->argv;
        myargc = argc;
        Z_Init();
        D_DoomMainSetup();
        g->init_ok = true;
    }
    g->jump = nullptr;
    g->init_done.store(true);
    return nullptr;
}

void start_init() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kInitStack);
    g->init_started = pthread_create(&g->init_thread, &attr, init_main, nullptr) == 0;
    if (!g->init_started) {
        show_message("DOOM\n\nCOULDN'T START");
    }
}

void finish_init() {
    pthread_join(g->init_thread, nullptr);
    g->init_started = false;
    if (!g->init_ok) {
        show_message(std::string("DOOM COULDN'T START\n\n") + g->fatal);
        return;
    }
    viper_frame_set_indexed(kWidth, kHeight, g->palette);
    g->palette_dirty.store(false);
    g->sound = doom_sound_start();
    g->state = State::Running;
    doom_memory_stats_t m;
    doom_memory_stats(&m);
    viper_log(
        "doom: ready; arena %u KB (%s), %u KB used; heap %u KB; tables %u KB fast, %u KB heap",
        unsigned(m.arena_size / 1024), m.arena_borrowed ? "lent" : "own",
        unsigned(m.arena_used / 1024), unsigned(m.heap_used / 1024), unsigned(m.static_fast / 1024),
        unsigned(m.static_heap / 1024));
}

// One frame of the game: D_DoomLoop()'s body, without the waits
void run_frame() {
    if (g->palette_dirty.exchange(false)) {
        viper_frame_set_indexed(kWidth, kHeight, g->palette);
    }
    if (D_WipeRunning()) {
        D_WipeStep();
        return;
    }
    I_StartFrame();
    D_BuildNewTiccmds(); // the tics due by the clock, each with its input
    int runtics = maketic - gametic;
    if (runtics <= 0) {
        return; // too soon: Doom draws when a tic has run
    }
    while (runtics--) {
        if (advancedemo) {
            D_DoAdvanceDemo();
        }
        M_Ticker();
        G_Ticker();
        gametic++;
    }
    if (players[displayplayer].mo) {
        S_UpdateSounds(players[displayplayer].mo);
    }
    D_Display();
}

void shutdown_engine() {
    doom_sound_stop();
    if (g->init_ok && !g->fatal[0]) {
        // The settings (prboom.cfg, with the saves): volume, autorun, the
        // screen size and the rest of the options menu
        M_SaveDefaults();
    }
    if (g->init_ok) {
        // As esp-box-emu's deinit_doom(): free the zone and forget the
        // pointers into it, so the next start (the simulator keeps globals)
        // allocates afresh
        D_WipeReset();
        Z_Close();
        R_ResetDrawSegs();
        R_ResetPlanes();
        R_ResetVisSprites();
        R_ResetViewMapping();
        R_ResetInterpolations();
        P_ResetSpechit();
        P_ResetIntercepts();
        P_ResetMapStarts();
        G_ResetBodyQueue();
        P_ResetBrainTargets();
        AM_ResetMarks();
        W_Done();
        states = nullptr; // in the zone, freed with it
    }
    doom_memory_shutdown();
}

// ---- input -----------------------------------------------------------------------

struct KeyMap {
    uint32_t button;
    int *game; // the key it presses in the game
    int *menu; // and while a menu is up (nullptr: the same)
};

// The keys each button presses. The D-pad's left and right strafe and L and
// R turn, as a gamepad's sticks would; in a menu the D-pad moves as arrows.
// The menu's arrows are the same key codes as turning, which is why a button
// presses one or the other, not both.
const KeyMap kKeyMap[] = {
    {VIPER_BUTTON_UP, &key_up, &key_menu_up},
    {VIPER_BUTTON_DOWN, &key_down, &key_menu_down},
    {VIPER_BUTTON_LEFT, &key_strafeleft, &key_menu_left},
    {VIPER_BUTTON_RIGHT, &key_straferight, &key_menu_right},
    {VIPER_BUTTON_L, &key_left, nullptr},
    {VIPER_BUTTON_R, &key_right, nullptr},
    {VIPER_BUTTON_A, &key_fire, &key_enter},
    {VIPER_BUTTON_B, &key_use, &key_backspace},
    {VIPER_BUTTON_X, &key_weapontoggle, nullptr},
    {VIPER_BUTTON_Y, &key_map, nullptr},
    {VIPER_BUTTON_START, &key_escape, nullptr},
    {VIPER_BUTTON_SELECT, &key_map, nullptr},
};
constexpr size_t kKeys = std::size(kKeyMap);

// The key each button's press sent, so its release lets go of the same one
// even if a menu opened or closed in between (-1 while it's up)
int s_held_key[kKeys];

void post_key(int key, bool down) {
    event_t ev = {};
    ev.type = down ? ev_keydown : ev_keyup;
    ev.data1 = key;
    D_PostEvent(&ev);
}

void reset_input() { std::fill(std::begin(s_held_key), std::end(s_held_key), -1); }

void post_input() {
    const uint32_t now = g->buttons;
    const uint32_t changed = now ^ g->posted_buttons;
    if (!changed) {
        return;
    }
    for (size_t i = 0; i < kKeys; i++) {
        const KeyMap &k = kKeyMap[i];
        if (!(changed & k.button)) {
            continue;
        }
        if (now & k.button) {
            const int key = (menuactive && k.menu) ? *k.menu : *k.game;
            s_held_key[i] = key;
            post_key(key, true);
        } else if (s_held_key[i] >= 0) {
            post_key(s_held_key[i], false);
            s_held_key[i] = -1;
        }
    }
    g->posted_buttons = now;
}

// ---- the app ---------------------------------------------------------------------

void create(void *) {
    g = new Doom();
    reset_input();
    viper_app_handle_back(1); // B opens doors; Doom's menu quits
    if (!doom_memory_init()) {
        show_message("DOOM\n\nNOT ENOUGH MEMORY");
        return;
    }
    g->wad = find_wad();
    if (g->wad.empty() || !file_exists(g->wad)) {
        show_message("DOOM\n\nNO WAD FILE\n\nPUT DOOM1.WAD IN ROMS/\nAND OPEN IT FROM HERE");
        return;
    }
    if (const char *data = viper_data_path()) {
        // The saves and prboom.cfg; the folder (and data/ above it) may not
        // be there yet
        g->save_dir = data;
        for (size_t i = 1; i <= g->save_dir.size(); i++) {
            if (i == g->save_dir.size() || g->save_dir[i] == '/') {
                mkdir(g->save_dir.substr(0, i).c_str(), 0775);
            }
        }
    }
    // For I_InitGraphics(); the engine draws here
    g->screen = static_cast<uint8_t *>(calloc(1, kWidth * kHeight));
    if (!g->screen) {
        show_message("DOOM\n\nNOT ENOUGH MEMORY");
        return;
    }
    draw_message("DOOM\n\nLOADING...", gfx::rgb_hex(0xc0c0c0));
    start_init();
}

void destroy(void) {
    if (!g) {
        return;
    }
    if (g->init_started) {
        pthread_join(g->init_thread, nullptr);
    }
    shutdown_engine();
    free(g->screen);
    viper_frame_set_indexed(0, 0, nullptr);
    delete g;
    g = nullptr;
}

void pause(void) {
    if (g && g->sound) {
        doom_sound_pause(true);
    }
}

void resume(void) {
    if (!g) {
        return;
    }
    if (g->state == State::Running) {
        viper_frame_set_indexed(kWidth, kHeight, g->palette);
        R_ForceRedraw();
    } else {
        g->message_drawn = false;
    }
    if (g->sound) {
        doom_sound_pause(false);
    }
}

uint32_t loop(const viper_input_t *now, const viper_input_t *previous) {
    switch (g->state) {
    case State::Loading:
        if (g->init_done.load()) {
            finish_init();
        }
        return 16;
    case State::Message:
        if (!g->message_drawn) {
            draw_message(g->message, gfx::rgb_hex(0xff6060));
            g->message_drawn = true;
        }
        if (viper_button_pressed(now, previous, VIPER_BUTTON_A) ||
            viper_button_pressed(now, previous, VIPER_BUTTON_B) ||
            viper_button_pressed(now, previous, VIPER_BUTTON_START)) {
            viper_app_exit();
        }
        return 33;
    case State::Quit:
        return 100;
    case State::Running:
        break;
    }

    g->buttons = now->buttons;
    jmp_buf jump;
    g->jump = &jump;
    if (setjmp(jump) == 0) {
        g->in_loop = true;
        const uint64_t t0 = viper_time_us();
        run_frame();
        g->engine_us += viper_time_us() - t0;
        if (++g->frames % 350 == 0) {
            doom_memory_stats_t m;
            doom_memory_stats(&m);
            viper_log("doom: %u us a frame; arena %u/%u KB (peak %u), heap %u KB (peak %u)",
                      unsigned(g->engine_us / 350), unsigned(m.arena_used / 1024),
                      unsigned(m.arena_size / 1024), unsigned(m.arena_peak / 1024),
                      unsigned(m.heap_used / 1024), unsigned(m.heap_peak / 1024));
            g->engine_us = 0;
        }
        if (g->sound) {
            doom_sound_pump();
        }
    }
    g->in_loop = false;
    g->jump = nullptr;
    // A tic is 1000/35 ms; come back in time for the next
    return g->state == State::Running ? 10 : 33;
}

const viper_app_t app = {
    .magic = VIPER_APP_MAGIC,
    .abi_version = VIPER_APP_ABI_VERSION,
    .render_engine = VIPER_RENDER_VIDEO_BLIT,
    .create = create,
    .resume = resume,
    .pause = pause,
    .destroy = destroy,
    .loop = loop,
};

} // namespace

VIPER_APP(app)

#ifdef VIPER_APP_STATIC_NAME
// What the simulator's tests look at (sim/tests/test_boot.cpp, "Doom ...")
extern "C" {
// -1 not running, 0 loading, 1 running, 2 showing a message, 3 quitting
int viper_sim_doom_state(void) { return g ? static_cast<int>(g->state) : -1; }
const char *viper_sim_doom_message(void) { return g ? g->message.c_str() : ""; }
int viper_sim_doom_gametic(void) { return gametic; }
int viper_sim_doom_menu_active(void) { return menuactive ? 1 : 0; }
int viper_sim_doom_player_angle(void) {
    return players && players[consoleplayer].mo ? int(players[consoleplayer].mo->angle >> 24) : -1;
}
int viper_sim_doom_in_level(void) { return gamestate == GS_LEVEL && !demoplayback ? 1 : 0; }
int viper_sim_doom_wiping(void) { return D_WipeRunning() ? 1 : 0; }
}
#endif

// ---- the engine's platform functions (i_system.h, i_video.h, i_main.h) ----------

extern "C" {

// Leaves the engine (I_Error()): back to setjmp() in loop() or the init
// thread, which shows the message
void doom_fatal(const char *message) {
    if (!g || !g->jump) {
        viper_log("doom: fatal outside the engine: %s", message);
        return;
    }
    snprintf(g->fatal, sizeof(g->fatal), "%s", message);
    if (g->state == State::Running) {
        doom_sound_stop();
        g->sound = false;
        show_message(std::string("DOOM STOPPED\n\n") + message);
    }
    longjmp(*g->jump, 1);
}

void I_Init(void) {}

void I_SafeExit(int rc) {
    (void)rc;
    viper_app_exit();
    if (g) {
        g->state = State::Quit;
        if (g->jump) {
            longjmp(*g->jump, 1);
        }
    }
}

const char *I_DoomExeDir(void) {
    // prboom.cfg (the settings) lives with the saves
    return g && !g->save_dir.empty() ? g->save_dir.c_str() : ".";
}

const char *I_SigString(char *buf, size_t sz, int signum) {
    snprintf(buf, sz, "signal %d", signum);
    return buf;
}

int I_GetTimeMS(void) { return int(viper_time_us() / 1000); }

int I_GetTime(void) { return int(viper_time_us() * TICRATE / 1000000); }

void I_uSleep(unsigned long usecs) {
    // The engine sleeps only where it waits for something (sounds to finish
    // when quitting); the app's loop() does the pacing
    (void)usecs;
}

bool I_StartDisplay(void) { return true; }
void I_EndDisplay(void) {}
void I_StartFrame(void) {}
void I_UpdateNoBlit(void) {}

void I_StartTic(void) {
    if (g) {
        post_input();
    }
}

void I_InitGraphics(void) {
    for (int i = 0; i < 3; i++) {
        screens[i].width = SCREENWIDTH;
        screens[i].height = SCREENHEIGHT;
        screens[i].byte_pitch = SCREENWIDTH;
    }
    screens[0].data = g->screen;
    screens[0].not_on_heap = true;
    screens[4].width = SCREENWIDTH;
    screens[4].height = ST_SCALED_HEIGHT + 1;
    screens[4].byte_pitch = SCREENWIDTH;
}

void I_UpdateVideoMode(void) {}
void I_ShutdownGraphics(void) {}

void I_SetPalette(int pal) {
    auto *p = static_cast<uint16_t *>(V_BuildPalette(pal, 16));
    if (!p) {
        return;
    }
    memcpy(g->palette, p, sizeof(g->palette));
    Z_Free(p); // V_BuildPalette() allocates it from the zone
    g->palette_dirty.store(true);
}

void I_FinishUpdate(void) {
    if (!g || !g->in_loop) {
        return; // during setup: nothing to show yet
    }
    if (g->palette_dirty.exchange(false)) {
        viper_frame_set_indexed(kWidth, kHeight, g->palette);
    }
    auto *frame = reinterpret_cast<uint8_t *>(viper_frame_begin());
    if (frame) {
        memcpy(frame, g->screen, kWidth * kHeight);
        viper_frame_end(1);
    }
}

// The haptics esp-box-emu plays for what happens to the player (r_main.h);
// viper_haptic() does nothing when they're off
void R_PlayerFire(player_t *player) {
    switch (player->readyweapon) {
    case wp_shotgun:
    case wp_supershotgun:
    case wp_missile:
        viper_haptic(VIPER_HAPTIC_STRONG_CLICK);
        break;
    case wp_bfg:
        viper_haptic(VIPER_HAPTIC_BUZZ1);
        break;
    case wp_fist:
    case wp_chainsaw:
        break;
    default:
        viper_haptic(VIPER_HAPTIC_SHARP_CLICK);
        break;
    }
}
void R_PlayerHurt(player_t *, int damage, int saved) {
    if (damage > 5) {
        viper_haptic(saved > 0 ? 70 : 75);
    } else if (damage > 0) {
        viper_haptic(saved > 0 ? 78 : VIPER_HAPTIC_TRANSITION_HUM_1);
    }
}
void R_PlayerInteract(player_t *, int) {}
void R_PlayerPickupWeapon(player_t *, int) { viper_haptic(29); }
void R_PlayerPickupAmmo(player_t *, ammotype_t, int) {}
void R_PlayerPickupHealth(player_t *, int) {}
void R_PlayerPickupArmor(player_t *, int) {}
void R_PlayerPickupCard(player_t *, card_t) { viper_haptic(VIPER_HAPTIC_DOUBLE_CLICK); }
void R_PlayerPickupPowerUp(player_t *, int) { viper_haptic(VIPER_HAPTIC_TRIPLE_CLICK); }

} // extern "C"
