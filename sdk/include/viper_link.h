#pragma once

/*
 * Link play for ELF apps: two to four units in the same room playing one game
 * over the radio (ESP-NOW). Design: docs/design/multiplayer.md ("ELF apps");
 * how to use it: apps/README.md ("Link play"); the example: apps/link_demo.
 *
 * The OS runs the lobby and draws it: Host / Find rooms, the room list (with
 * why a room can't be joined), the roster with ready marks, the game's
 * settings, Ready and Start. It is the lobby built-in games show. While it's
 * up the app calls viper_link_lobby() once a frame and draws nothing. Once
 * the host starts, every unit gets the same start parameters
 * (viper_link_start(): a seed, tick 0's time on the session clock, the
 * settings) and the game runs, sending messages (viper_link_send(),
 * viper_link_poll()) or, for a lockstep game, running game::InputSync over
 * viper_link_sync.hpp.
 *
 *     create():   viper_link_config_t c = {.size = sizeof c, .min_players = 2,
 *                                          .max_players = 2};
 *                 viper_link_open(&c);
 *     loop():     int r = viper_link_lobby(now, previous);
 *                 if (r == VIPER_LINK_LOBBY_SHOWN) return 16;   (it drew the frame)
 *                 if (r == VIPER_LINK_LOBBY_EXIT) ...           (B: link play closed)
 *                 if (r == VIPER_LINK_LOBBY_PLAYING) {
 *                     viper_link_event_t e = {.size = sizeof e};
 *                     while (viper_link_poll(&e)) ...           (every frame)
 *                     ... play, viper_link_send() ...
 *                 }
 *     destroy():  viper_link_close();   (closing the app closes it anyway)
 *
 * Rules:
 *   - One app has link play open at a time, from viper_link_open() until
 *     viper_link_close() or the app closes. The radio is held only from Host
 *     or Find rooms in the lobby until the room is left: WiFi is off meanwhile
 *     and comes back after.
 *   - Rooms are the app's own: the OS takes the app id from its bundle id, so
 *     only the same app sees them. content_id and app_protocol must also
 *     match to join.
 *   - Structs passed in or out start with `size`, which the app sets to
 *     sizeof before the call. The OS reads or writes no more than that and,
 *     on the way out, sets it to how much it wrote, so a struct can grow at
 *     the end without breaking apps (VIPER_LINK_HAS()). A size smaller than
 *     this header's version is refused (VIPER_LINK_ERR_ARG).
 *   - Nothing here keeps a pointer the app passed, and nothing returns a
 *     pointer into the OS: what arrives is copied into the app's structs.
 *   - Every argument is checked: a bad slot, channel, type, size or state
 *     returns an error rather than doing anything. Main task (loop() and the
 *     other callbacks) only.
 *
 * Plain C, fixed-size types and explicit layouts: the firmware and apps
 * share these structs, so a field never moves (static_asserts in
 * components/system/src/elf_link.cpp hold them).
 */

#include <stddef.h>
#include <stdint.h>

#include "viper_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIPER_LINK_MAX_PLAYERS      4    /* a room holds 2 to 4, the host in slot 0 */
#define VIPER_LINK_MAX_MESSAGE      200  /* bytes in a message body */
#define VIPER_LINK_MAX_SETTINGS     64   /* bytes of game settings the host shares */
#define VIPER_LINK_MAX_SETTING_ROWS 4    /* settings the lobby shows and edits */
#define VIPER_LINK_MAX_SYNC_PACKET  163  /* game::InputSync::kMaxPacket */
#define VIPER_LINK_FIRST_RESERVED_TYPE 0xE0 /* message types from here up are the OS's */
#define VIPER_LINK_ALL      0xFF /* viper_link_send() to every other player */
#define VIPER_LINK_NO_SLOT  0xFF /* not in a room */

/* `field` of a struct the OS filled in is there (an older OS may not write it) */
#define VIPER_LINK_HAS(s, field) \
    ((s)->size >= offsetof(__typeof__(*(s)), field) + sizeof((s)->field))

/* Results. Functions that return int return VIPER_LINK_OK or one of these. */
#define VIPER_LINK_OK               0
#define VIPER_LINK_ERR_ARG         (-1) /* a NULL, a size, slot, channel or type out of range */
#define VIPER_LINK_ERR_STATE       (-2) /* not now: link play isn't open, the game hasn't started, ... */
#define VIPER_LINK_ERR_BUSY        (-3) /* link play is already open */
#define VIPER_LINK_ERR_TOO_LARGE   (-4) /* a body over VIPER_LINK_MAX_MESSAGE */
#define VIPER_LINK_ERR_NO_PLAYER   (-5) /* nobody in that slot (or it's this unit's own) */
#define VIPER_LINK_ERR_QUEUE_FULL  (-6) /* reliable: that player already has 8 messages waiting */
#define VIPER_LINK_ERR_SEND_FAILED (-7) /* unreliable: the radio refused it (as good as lost) */

/* ---- Opening link play ---- */

/* A game setting the lobby shows and the host changes with Left and Right:
 * one byte of the settings, from min to max. */
typedef struct {
    char name[20];    /* "Points to win" (NUL-terminated) */
    char labels[44];  /* names for min, min + 1, ... separated by '|', e.g.
                       * "Easy|Normal|Hard"; empty shows the number */
    uint8_t offset;   /* which byte of viper_link_config_t.settings */
    uint8_t min;
    uint8_t max;
    uint8_t reserved_;
} viper_link_setting_t;

typedef struct {
    uint32_t size;          /* sizeof(viper_link_config_t) */
    uint32_t content_id;    /* the app's own, e.g. a level pack's checksum; must match to join */
    uint16_t app_protocol;  /* the app's own compatibility number; must match to join */
    uint8_t min_players;    /* Start needs this many in the room: 2 to max_players */
    uint8_t max_players;    /* room size, 2 to VIPER_LINK_MAX_PLAYERS */
    uint32_t tick_us;       /* lockstep games: the tick, 1000 to 1000000; 0 means 16667 (60 Hz) */
    uint8_t input_size;     /* lockstep games: bytes per player per tick, 1 to 8; 0 means 2 */
    uint8_t settings_size;  /* bytes of settings, 0 to VIPER_LINK_MAX_SETTINGS */
    uint8_t setting_count;  /* rows of setting_rows the lobby shows, 0 to VIPER_LINK_MAX_SETTING_ROWS */
    uint8_t reserved_;
    uint8_t settings[VIPER_LINK_MAX_SETTINGS]; /* the host's settings to start with */
    viper_link_setting_t setting_rows[VIPER_LINK_MAX_SETTING_ROWS];
} viper_link_config_t;

/* Opens link play with the lobby on its first page (Host / Find rooms). The
 * radio isn't touched until the player picks one. VIPER_LINK_OK,
 * VIPER_LINK_ERR_ARG for a bad config, or VIPER_LINK_ERR_BUSY if it's
 * already open. */
int viper_link_open(const viper_link_config_t *config);

/* Leaves the room (the others see this unit leave, or the room end if it's
 * the host), gives the radio back and forgets everything. Safe any time. */
void viper_link_close(void);

/* ---- The lobby ---- */

#define VIPER_LINK_LOBBY_SHOWN   0  /* the lobby is on screen: it drew this frame */
#define VIPER_LINK_LOBBY_PLAYING 1  /* the game has started: play (viper_link_start()) */
#define VIPER_LINK_LOBBY_EXIT    2  /* B on the lobby's first page: link play is closed */

/* Once a frame until it returns PLAYING: runs the lobby with this frame's
 * buttons and draws it, the whole screen (a VIPER_RENDER_VIDEO_BLIT app's
 * frame, or a canvas over an LVGL app's container). B is the lobby's while
 * it's up. Returns one of the above, or VIPER_LINK_ERR_STATE if link play
 * isn't open. While playing it returns PLAYING and does nothing, so a game
 * can call it every frame; after VIPER_LINK_EVENT_BACK_TO_ROOM or
 * VIPER_LINK_EVENT_ENDED the lobby is back. */
int viper_link_lobby(const viper_input_t *now, const viper_input_t *previous);

/* What every unit starts with: the same on each (but local_slot). */
typedef struct {
    uint32_t size;          /* sizeof(viper_link_start_t) */
    uint32_t seed;          /* seed the game's random numbers with it */
    int64_t start_us;       /* tick 0 is due at this time on the session clock (viper_link_now_us()) */
    uint32_t tick_us;       /* lockstep: the config's tick */
    uint8_t input_delay;    /* lockstep: ticks of input delay, from the round trip */
    uint8_t input_size;     /* lockstep: the config's input size */
    uint8_t players;        /* a bit per slot in the game (slot 0 is bit 0) */
    uint8_t local_slot;     /* this unit's slot */
    uint32_t match_id;      /* a hash of all of the above but local_slot, and the settings */
    uint8_t settings_size;
    uint8_t reserved_[3];
    uint8_t settings[VIPER_LINK_MAX_SETTINGS]; /* the host's, as they were at Start */
} viper_link_start_t;

/* The start parameters of the game being played. VIPER_LINK_ERR_STATE before
 * the lobby has returned PLAYING. */
int viper_link_start(viper_link_start_t *out);

/* Host, in a room: replaces the settings (0 to VIPER_LINK_MAX_SETTINGS bytes)
 * with the app's own, for settings the lobby's rows can't edit: a level
 * chosen on the app's own screen, the next round after a rematch. Every
 * guest gets them, and their Ready is cleared (they're ready for these
 * settings, not the old ones); the same settings change nothing. The rows
 * show the new values. The room keeps running while the app draws its own
 * screen instead of calling viper_link_lobby(), but B closes the app then
 * unless it took B (viper_app_handle_back()). VIPER_LINK_ERR_STATE unless
 * this unit hosts a room that hasn't started (or is back from a game). */
int viper_link_set_settings(const void *settings, size_t size);

/* Host, while playing: everyone back to the room for another round, not
 * ready, settings as they were. Every unit gets VIPER_LINK_EVENT_BACK_TO_ROOM;
 * the next Start has a new seed. VIPER_LINK_ERR_STATE if not the host or not
 * playing. */
int viper_link_rematch(void);

/* ---- Who's here ---- */

#define VIPER_LINK_CLOSED  0 /* not open */
#define VIPER_LINK_LOBBY   1 /* the lobby is up (any page) */
#define VIPER_LINK_PLAYING 2

#define VIPER_LINK_PLAYER_HOST    (1u << 0)
#define VIPER_LINK_PLAYER_LOCAL   (1u << 1) /* this unit */
#define VIPER_LINK_PLAYER_READY   (1u << 2) /* in the lobby: ready (the host always is) */
#define VIPER_LINK_PLAYER_LAGGING (1u << 3) /* nothing heard from it for half a second */

typedef struct {
    uint8_t slot;
    uint8_t flags;          /* VIPER_LINK_PLAYER_* */
    uint8_t loss_pct;       /* keepalives from it lost, over the last 32 */
    uint8_t reserved_;
    uint32_t rtt_us;        /* round trip to it, smoothed; 0 for this unit */
    char name[20];          /* the player name from Settings (NUL-terminated UTF-8) */
} viper_link_player_t;

typedef struct {
    uint32_t size;          /* sizeof(viper_link_status_t) */
    uint8_t state;          /* VIPER_LINK_CLOSED, _LOBBY or _PLAYING */
    uint8_t local_slot;     /* VIPER_LINK_NO_SLOT outside a room */
    uint8_t is_host;
    uint8_t player_count;
    viper_link_player_t players[VIPER_LINK_MAX_PLAYERS]; /* in slot order */
} viper_link_status_t;

/* Where link play is and who's in the room. VIPER_LINK_OK (with state
 * VIPER_LINK_CLOSED when it isn't open) or VIPER_LINK_ERR_ARG. */
int viper_link_status(viper_link_status_t *out);

/* The session clock: the host's clock in microseconds, the same instant on
 * every unit (to within a millisecond or so). Time the game's start by it.
 * Outside a room, the local clock (viper_time_us()). */
int64_t viper_link_now_us(void);

/* ---- Playing: messages ---- */

#define VIPER_LINK_UNRELIABLE 0 /* may be lost; an older one than one delivered is dropped */
#define VIPER_LINK_RELIABLE   1 /* arrives once, in order per sender; resent until acknowledged */

/* Sends `size` bytes (0 to VIPER_LINK_MAX_MESSAGE) of message `type` (0 to
 * VIPER_LINK_FIRST_RESERVED_TYPE - 1, the app's own numbering) to the player
 * in `to_slot`, or to every other player with VIPER_LINK_ALL. Unreliable for
 * what's resent anyway (positions, snapshots, every frame), reliable for
 * events (lines cleared, a turn played, game over). Only while playing.
 * VIPER_LINK_OK or an error. */
int viper_link_send(uint8_t to_slot, int channel, uint8_t type, const void *body, size_t size);

/* Event types */
#define VIPER_LINK_EVENT_MESSAGE          1 /* from slot: channel, message_type, body */
#define VIPER_LINK_EVENT_UNDELIVERED      2 /* a reliable message_type to slot may not have
                                             * arrived (reason: VIPER_LINK_UNDELIVERED_*) */
#define VIPER_LINK_EVENT_PLAYER_LEFT      3 /* slot left or went silent (reason) */
#define VIPER_LINK_EVENT_PLAYER_LAGGING   4 /* nothing heard from slot for half a second */
#define VIPER_LINK_EVENT_PLAYER_RECOVERED 5 /* heard from it again */
#define VIPER_LINK_EVENT_BACK_TO_ROOM     6 /* the host called a rematch: the lobby is back */
#define VIPER_LINK_EVENT_ENDED            7 /* the room is gone (reason): the lobby is back,
                                             * on its first page */

/* Why: a player left or the room ended (and why a room can't be joined) */
#define VIPER_LINK_REASON_NONE      0
#define VIPER_LINK_REASON_LEFT      7  /* said goodbye */
#define VIPER_LINK_REASON_TIMED_OUT 8  /* went silent for 3 seconds */
#define VIPER_LINK_REASON_HOST_LEFT 9  /* the host closed the room */
#define VIPER_LINK_REASON_HOST_LOST 10 /* the host went silent */
#define VIPER_LINK_REASON_REMOVED   11 /* the host dropped this unit */

/* Why a reliable message may not have arrived */
#define VIPER_LINK_UNDELIVERED_PEER_LEFT     1
#define VIPER_LINK_UNDELIVERED_SESSION_ENDED 2
#define VIPER_LINK_UNDELIVERED_TIMEOUT       3 /* 5 s without an acknowledgement */

typedef struct {
    uint32_t size;          /* sizeof(viper_link_event_t) */
    uint8_t type;           /* VIPER_LINK_EVENT_* */
    uint8_t slot;           /* the player it's about: sender, addressee, who left */
    uint8_t reason;         /* VIPER_LINK_REASON_*, or VIPER_LINK_UNDELIVERED_* */
    uint8_t channel;        /* MESSAGE: VIPER_LINK_UNRELIABLE or VIPER_LINK_RELIABLE */
    uint8_t message_type;   /* MESSAGE, UNDELIVERED */
    uint8_t reserved_;
    uint16_t body_size;     /* MESSAGE */
    uint32_t reserved2_;
    int64_t received_us;    /* MESSAGE: when it arrived, on viper_time_us()'s clock */
    uint8_t body[VIPER_LINK_MAX_MESSAGE];
} viper_link_event_t;

/* The next event, copied into `out`: 1, or 0 when there are none. Call it
 * every frame while playing, until it returns 0: that's also how the end of
 * the room and the host's rematch are noticed. Events are queued only while
 * playing; the lobby handles everything before that. */
int viper_link_poll(viper_link_event_t *out);

/* ---- Playing: game::InputSync (lockstep) ----
 *
 * These carry InputSync's packets; viper_link_sync.hpp (with
 * set(VIPER_LIBS game)) wraps them as a game::SyncPort, so apps don't call
 * them directly. Only while playing. */

/* One packet (1 to VIPER_LINK_MAX_SYNC_PACKET bytes) to the player in
 * `to_slot`, unreliably. VIPER_LINK_OK or an error. */
int viper_link_sync_send(uint8_t to_slot, const void *packet, size_t size);

/* The next packet that arrived: copies up to `size` bytes into `buf`, sets
 * *from_slot, and returns the packet's length, or 0 when there are none (or
 * an error). The OS keeps the last 16; older ones are dropped. */
int viper_link_sync_receive(uint8_t *from_slot, void *buf, size_t size);

#ifdef __cplusplus
}
#endif
