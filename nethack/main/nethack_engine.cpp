// NetHack's engine thread, the turn between it and loop(), and the C side's
// calls into the game model (port/nh_port.h). See nethack_engine.hpp.

#include "nethack_engine.hpp"

#include <pthread.h>
#include <setjmp.h>
#include <time.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "port/nh_port.h"
#include "viper_app.h"

// The simulator links the app into its own program, so NetHack's variables
// keep their values from one game to the next (a device loads the app
// afresh). They're in sections of their own there (port/nh_sim.h), which
// these bound.
#if defined(VIPER_APP_STATIC_NAME) && defined(__clang__)
#define NH_RESTART_STATE 1
#ifdef __APPLE__
extern char nh_data_start __asm("section$start$__DATA$__nh_data");
extern char nh_data_end __asm("section$end$__DATA$__nh_data");
extern char nh_bss_start __asm("section$start$__DATA$__nh_bss");
extern char nh_bss_end __asm("section$end$__DATA$__nh_bss");
#else
extern "C" char __start_nh_data, __stop_nh_data, __start_nh_bss, __stop_nh_bss;
#define nh_data_start __start_nh_data
#define nh_data_end __stop_nh_data
#define nh_bss_start __start_nh_bss
#define nh_bss_end __stop_nh_bss
#endif
#endif

namespace nethack {
namespace {

// The engine thread's stack. NetHack's deepest calls are Lua's, making a
// level. The simulator's sanitizers make frames many times bigger.
#ifdef VIPER_APP_STATIC_NAME
constexpr size_t kEngineStack = 4 * 1024 * 1024;
#else
constexpr size_t kEngineStack = 64 * 1024;
#endif

// Closing the app saves the game: each key the engine asks for then is ESC,
// then S, until it has saved and quit. Past this many, it gives up (a prompt
// that takes neither) and the game is lost.
constexpr int kStopKeys = 400;

// A block NetHack or Lua allocated: kept in a list, so that ending the game
// frees them all, wherever the engine was. Four words (16 bytes on a
// device), so what follows is as aligned as malloc()'s
struct Block {
    Block *prev;
    Block *next;
    size_t size;
    size_t unused;
};

struct Engine {
    std::string app_dir, data_dir, name;

    pthread_t thread{};
    bool thread_started = false;
    pthread_mutex_t mutex{};
    pthread_cond_t cond{};
    bool engine_turn = true; // the engine is running (it starts so)
    bool finished = false;   // the engine thread has returned
    bool stopping = false;   // engine_stop(): save and quit
    int stop_keys = 0;
    jmp_buf exit_jump; // where nhv_exit() goes
    std::string error;
    std::string last_log;

    Model model;
    std::string queued; // keys sent ahead (engine_queue_keys())

    // The player's answer, handed over with the turn
    int key = 0;
    char yn = 0;
    std::string line;
    std::vector<int> menu_indexes;
    bool menu_cancel = false;

    Block blocks{};
    size_t heap_used = 0;
    size_t heap_peak = 0;
};

Engine *g = nullptr;
unsigned long fixed_seed = 0; // engine_set_seed()

#ifdef NH_RESTART_STATE
std::vector<char> g_initial_data;
bool g_saved = false;

__attribute__((no_sanitize("address"))) void copy_bytes(volatile char *to,
                                                        const volatile char *from, size_t n) {
    for (size_t i = 0; i < n; i++) {
        to[i] = from[i];
    }
}
#endif

// Puts NetHack's variables back as they were before the first game (the
// simulator), or says whether this is the first game (a simulator built
// without Clang, which can only run one). A device always starts afresh.
bool restart_state() {
#ifdef NH_RESTART_STATE
    const size_t data = &nh_data_end - &nh_data_start;
    const size_t bss = &nh_bss_end - &nh_bss_start;
    if (!g_saved) {
        g_initial_data.resize(data);
        copy_bytes(g_initial_data.data(), &nh_data_start, data);
        g_saved = true;
        return true;
    }
    copy_bytes(&nh_data_start, g_initial_data.data(), data);
    std::vector<char> zeros(bss);
    copy_bytes(&nh_bss_start, zeros.data(), bss);
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

// ---- the turn ----

// Hands the turn to loop() with a request, and waits for its answer. False
// while stopping: the caller answers for the player.
bool wait_answer(const Request &request) {
    if (g->stopping) {
        return false;
    }
    g->model.request = request;
    g->model.request_id++;
    g->model.changes++;
    pthread_mutex_lock(&g->mutex);
    g->engine_turn = false;
    pthread_cond_broadcast(&g->cond);
    while (!g->engine_turn) {
        pthread_cond_wait(&g->cond, &g->mutex);
    }
    pthread_mutex_unlock(&g->mutex);
    g->model.request = Request{};
    g->model.changes++;
    return !g->stopping;
}

void give_turn() {
    pthread_mutex_lock(&g->mutex);
    g->engine_turn = true;
    pthread_cond_broadcast(&g->cond);
    pthread_mutex_unlock(&g->mutex);
}

void *engine_main(void *) {
    if (setjmp(g->exit_jump) == 0) {
        nhv_nethack_main(g->app_dir.c_str(), g->data_dir.c_str(), g->name.c_str());
    }
    pthread_mutex_lock(&g->mutex);
    g->finished = true;
    g->engine_turn = false;
    pthread_cond_broadcast(&g->cond);
    pthread_mutex_unlock(&g->mutex);
    return nullptr;
}

void free_blocks() {
    Block *b = g->blocks.next;
    while (b && b != &g->blocks) {
        Block *next = b->next;
        std::free(b);
        b = next;
    }
    g->blocks.next = g->blocks.prev = &g->blocks;
    g->heap_used = 0;
}

bool contains_ci(const std::string &s, const char *word) {
    std::string lower(s);
    for (char &c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower.find(word) != std::string::npos;
}

Window *window(int win) { return g->model.window(win); }

} // namespace

// ---- the app's side ----

bool engine_start(const std::string &app_dir, const std::string &data_dir,
                  const std::string &hero_name) {
    if (g) {
        engine_stop();
    }
    g = new Engine;
    g->blocks.next = g->blocks.prev = &g->blocks;
    g->app_dir = app_dir;
    g->data_dir = data_dir;
    g->name = hero_name;
    pthread_mutex_init(&g->mutex, nullptr);
    pthread_cond_init(&g->cond, nullptr);
    if (!restart_state()) {
        g->error = "This simulator can run NetHack once: restart it to play again (or build it "
                   "with Clang)";
        g->finished = true;
        return false;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kEngineStack);
    g->thread_started = pthread_create(&g->thread, &attr, engine_main, nullptr) == 0;
    if (!g->thread_started) {
        g->error = "Couldn't start NetHack";
        g->finished = true;
    }
    return g->thread_started;
}

void engine_stop() {
    if (!g) {
        return;
    }
    if (g->thread_started) {
        pthread_mutex_lock(&g->mutex);
        g->stopping = true;
        g->engine_turn = true;
        pthread_cond_broadcast(&g->cond);
        pthread_mutex_unlock(&g->mutex);
        pthread_join(g->thread, nullptr);
        g->thread_started = false;
    }
    viper_log("nethack: stopped; heap peak %u KB", unsigned(g->heap_peak / 1024));
    free_blocks();
    pthread_cond_destroy(&g->cond);
    pthread_mutex_destroy(&g->mutex);
    delete g;
    g = nullptr;
}

bool engine_run(long wait_ms) {
    if (!g) {
        return true;
    }
    pthread_mutex_lock(&g->mutex);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += wait_ms / 1000;
    deadline.tv_nsec += (wait_ms % 1000) * 1000000L;
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
    pthread_mutex_unlock(&g->mutex);
    if (g->finished && g->thread_started) {
        pthread_join(g->thread, nullptr);
        g->thread_started = false;
    }
    return waiting;
}

bool engine_waiting() {
    if (!g) {
        return false;
    }
    pthread_mutex_lock(&g->mutex);
    const bool waiting = !g->engine_turn && !g->finished;
    pthread_mutex_unlock(&g->mutex);
    return waiting;
}

bool engine_finished() { return !g || g->finished; }

const std::string &engine_error() {
    static const std::string none;
    return g ? g->error : none;
}

const Model &engine_model() {
    static const Model empty;
    return g ? g->model : empty;
}

void engine_answer_key(int key) {
    g->key = key;
    give_turn();
}

void engine_answer_yn(char key) {
    g->yn = key;
    give_turn();
}

void engine_answer_line(const std::string &line) {
    g->line = line;
    give_turn();
}

void engine_answer_menu(const std::vector<int> &indexes, bool cancel) {
    g->menu_indexes = indexes;
    g->menu_cancel = cancel;
    give_turn();
}

void engine_answer_continue() { give_turn(); }

void engine_set_seed(unsigned long seed) { fixed_seed = seed; }

void engine_queue_keys(const std::string &keys) {
    if (g) {
        g->queued += keys;
    }
}

size_t engine_heap_used() { return g ? g->heap_used : 0; }
size_t engine_heap_peak() { return g ? g->heap_peak : 0; }

} // namespace nethack

// ---- NetHack's side (port/nh_port.h), on the engine's thread ----

using nethack::g;
using nethack::wait_answer;

extern "C" {

void nhv_map_clear(void) { g->model.clear_map(); }

void nhv_map_glyph(int x, int y, int glyph, int ch, int color, unsigned flags, int tile) {
    if (x < 0 || x >= nethack::kCols || y < 0 || y >= nethack::kRows) {
        return;
    }
    nethack::Cell &c = g->model.cell(x, y);
    c.glyph = glyph;
    c.ch = static_cast<char>(ch >= 32 && ch < 127 ? ch : '?');
    c.color = static_cast<uint8_t>(color & 15);
    c.flags = static_cast<uint8_t>(flags);
    c.tile = static_cast<int16_t>(tile);
    g->model.changes++;
}

void nhv_map_hero(int x, int y) {
    g->model.hero_x = x;
    g->model.hero_y = y;
    g->model.changes++;
}

void nhv_message(const char *text) {
    viper_log("nethack: %s", text);
    g->model.add_message(text);
}

void nhv_more(void) {
    if (g->model.new_messages == 0) {
        return;
    }
    nethack::Request r;
    r.ask = nethack::Ask::More;
    wait_answer(r);
    g->model.new_messages = 0;
}

void nhv_status(const char *const *lines, int count) {
    g->model.status.assign(lines, lines + count);
    g->model.changes++;
}

int nhv_window_create(int kind) { return g->model.create_window(kind); }

void nhv_window_destroy(int win) { g->model.destroy_window(win); }

void nhv_window_clear(int win) {
    if (nethack::Window *w = nethack::window(win)) {
        w->lines.clear();
        w->items.clear();
        w->prompt.clear();
        g->model.changes++;
    }
}

void nhv_window_text(int win, int attr, const char *text) {
    (void)attr;
    if (nethack::Window *w = nethack::window(win)) {
        w->lines.emplace_back(text);
        nethack::MenuItem item;
        item.text = text;
        w->items.push_back(item); // a menu shows text lines among its items
        g->model.changes++;
    }
}

void nhv_window_show(int win) {
    nethack::Request r;
    r.ask = nethack::Ask::Text;
    r.window = win;
    wait_answer(r);
}

void nhv_menu_start(int win) {
    if (nethack::Window *w = nethack::window(win)) {
        w->items.clear();
        w->lines.clear();
        w->prompt.clear();
        w->menu = true;
        g->model.changes++;
    }
}

void nhv_menu_add(int win, int index, char accelerator, int attr, int color, int selected,
                  const char *text) {
    if (nethack::Window *w = nethack::window(win)) {
        nethack::MenuItem item;
        item.index = index;
        item.accelerator = accelerator;
        item.attr = attr;
        item.color = color;
        item.selected = selected != 0;
        item.text = text;
        w->items.push_back(std::move(item));
        g->model.changes++;
    }
}

void nhv_menu_end(int win, const char *prompt) {
    if (nethack::Window *w = nethack::window(win)) {
        w->prompt = prompt ? prompt : "";
        g->model.changes++;
    }
}

int nhv_menu_select(int win, int how, int *indexes, long *counts, int max) {
    nethack::Request r;
    r.ask = nethack::Ask::Menu;
    r.window = win;
    r.pick = how;
    if (!wait_answer(r) || g->menu_cancel) {
        return -1;
    }
    int n = 0;
    for (int index : g->menu_indexes) {
        if (n < max) {
            indexes[n] = index;
            counts[n] = -1;
            n++;
        }
    }
    return n;
}

int nhv_get_key(void) {
    if (!g->queued.empty()) {
        const int key = static_cast<unsigned char>(g->queued.front());
        g->queued.erase(g->queued.begin());
        return key;
    }
    nethack::Request r;
    r.ask = nethack::Ask::Key;
    if (!wait_answer(r)) {
        // Closing: save and quit
        if (++g->stop_keys > nethack::kStopKeys) {
            nhv_exit(0);
        }
        return g->stop_keys % 2 ? nethack::kEsc : 'S';
    }
    g->model.new_messages = 0;
    return g->key;
}

char nhv_yn(const char *query, const char *choices, char def) {
    // A key the app sent ahead answers it ("o" then a direction)
    if (!g->queued.empty()) {
        const char key = g->queued.front();
        g->queued.erase(g->queued.begin());
        return key;
    }
    nethack::Request r;
    r.ask = nethack::Ask::YesNo;
    r.query = query ? query : "";
    r.choices = choices ? choices : "";
    r.def = def;
    if (r.choices.empty()) {
        r.choices = nethack::choices_in_query(r.query);
    }
    if (!wait_answer(r)) {
        if (++g->stop_keys > nethack::kStopKeys) {
            nhv_exit(0);
        }
        if (nethack::contains_ci(r.query, "save")) {
            return 'y';
        }
        return r.choices.find('n') != std::string::npos ? 'n' : nethack::kEsc;
    }
    g->model.new_messages = 0;
    return g->yn;
}

void nhv_getlin(const char *query, char *buf, size_t size) {
    if (size == 0) {
        return;
    }
    nethack::Request r;
    r.ask = nethack::Ask::Line;
    r.query = query ? query : "";
    std::string line(1, nethack::kEsc);
    if (wait_answer(r)) {
        line = g->line;
        g->model.new_messages = 0;
    } else if (++g->stop_keys > nethack::kStopKeys) {
        nhv_exit(0);
    }
    const size_t n = std::min(line.size(), size - 1);
    std::memcpy(buf, line.data(), n);
    buf[n] = '\0';
}

void nhv_context(unsigned here, unsigned doors) {
    if (g->model.here != here || g->model.doors != doors) {
        g->model.here = here;
        g->model.doors = doors;
        g->model.changes++;
    }
}

void nhv_getpos(const char *goal) {
    g->model.getpos = goal != nullptr;
    g->model.getpos_goal = goal ? goal : "";
    g->model.changes++;
}

void nhv_map_cursor(int x, int y) {
    if (g->model.cursor_x != x || g->model.cursor_y != y) {
        g->model.cursor_x = x;
        g->model.cursor_y = y;
        g->model.changes++;
    }
}

int nhv_queued_line(char *buf, size_t size) {
    const size_t end = g->queued.find('\n');
    if (end == std::string::npos || size == 0) {
        return 0;
    }
    const size_t n = std::min(end, size - 1);
    std::memcpy(buf, g->queued.data(), n);
    buf[n] = '\0';
    g->queued.erase(0, end + 1);
    return 1;
}

void nhv_exit(int status) {
    if (status != 0 && g->error.empty()) {
        g->error = g->last_log.empty() ? "NetHack stopped with an error" : g->last_log;
    }
    longjmp(g->exit_jump, 1);
}

unsigned long nhv_random_seed(void) { return nethack::fixed_seed; }

void nhv_log(const char *text) {
    g->last_log = text;
    viper_log("nethack: %s", text);
}

// ---- memory (port/nh_sim.h) ----

void *nhv_malloc(size_t size) {
    if (!g) {
        return std::malloc(size);
    }
    auto *b = static_cast<nethack::Block *>(std::malloc(sizeof(nethack::Block) + size));
    if (!b) {
        return nullptr;
    }
    b->size = size;
    b->prev = &g->blocks;
    b->next = g->blocks.next;
    g->blocks.next->prev = b;
    g->blocks.next = b;
    g->heap_used += size;
    g->heap_peak = std::max(g->heap_peak, g->heap_used);
    return b + 1;
}

void nhv_free(void *ptr) {
    if (!ptr) {
        return;
    }
    if (!g) {
        std::free(ptr);
        return;
    }
    auto *b = static_cast<nethack::Block *>(ptr) - 1;
    b->prev->next = b->next;
    b->next->prev = b->prev;
    g->heap_used -= b->size;
    std::free(b);
}

void *nhv_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        return nullptr;
    }
    void *p = nhv_malloc(count * size);
    if (p) {
        std::memset(p, 0, count * size);
    }
    return p;
}

void *nhv_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return nhv_malloc(size);
    }
    if (size == 0) {
        nhv_free(ptr);
        return nullptr;
    }
    void *p = nhv_malloc(size);
    if (!p) {
        return nullptr;
    }
    const size_t old = (static_cast<nethack::Block *>(ptr) - 1)->size;
    std::memcpy(p, ptr, std::min(old, size));
    nhv_free(ptr);
    return p;
}

char *nhv_strdup(const char *s) {
    const size_t n = std::strlen(s) + 1;
    auto *p = static_cast<char *>(nhv_malloc(n));
    if (p) {
        std::memcpy(p, s, n);
    }
    return p;
}

} // extern "C"
