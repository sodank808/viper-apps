#pragma once

// gfx::ui::LobbyView: the link play lobby for games that draw their own
// frames (VideoBlit apps, and ELF games from Phase 3). The same screens as the
// LVGL lobby (ui::LobbyScreen): Host / Join, the room list with the reason a
// room can't be joined, the roster with ready marks and the host, the game's
// settings, Ready / Start, and a hint bar. Design: docs/design/multiplayer.md
// ("Lobby").
//
// It knows nothing of the link. What it shows is a LobbySnapshot, plain data
// with fixed-size text (no pointers, no C++ types beyond enums and arrays),
// so an ELF game can be handed the same layout through the C API later; a
// built-in game fills it from viper::link::Lobby with ui::lobby_snapshot()
// (components/ui/include/lobby_snapshot.hpp). What the player asks for comes
// back from update() as a LobbyCommand, which the app applies to the lobby
// (ui::lobby_apply()). The LVGL view uses the same update() and hints(), so
// the two behave alike.
//
//     gfx::ui::LobbyView view_;
//     gfx::ui::LobbySnapshot snap_;
//
//     // each frame
//     lobby_.update();
//     ui::lobby_snapshot(lobby_, &settings_, snap_);
//     ui::lobby_apply(lobby_, &settings_, view_.update(snap_, keys));
//     view_.draw(s, s.bounds(), snap_);
//
// Header-only and platform-free like the rest of gfx.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>

#include "gfx/font_unscii8.hpp"
#include "gfx/ui.hpp"

namespace gfx::ui {

inline constexpr int kLobbyMaxRooms = 8;
inline constexpr int kLobbyMaxPlayers = 4;
inline constexpr int kLobbyMaxSettings = 6;
inline constexpr int kLobbyMaxInfo = 7;
inline constexpr int kLobbyText = 24;

enum class LobbyPage : uint8_t { Menu, Browse, Joining, Room, Started };

struct LobbyRoomRow {
    char name[kLobbyText] = {};   // the host's name
    char detail[kLobbyText] = {}; // "1/2", or why it can't be joined ("FULL")
    bool joinable = false;
};

struct LobbyPlayerRow {
    char name[kLobbyText] = {};
    uint8_t slot = 0;
    bool host = false;
    bool local = false; // this unit
    bool ready = false;
    bool lagging = false;
};

struct LobbySettingRow {
    char name[kLobbyText] = {};
    char value[kLobbyText] = {};
};

// Everything the lobby screens show, as plain data
struct LobbySnapshot {
    LobbyPage page = LobbyPage::Menu;
    bool is_host = false;
    bool local_ready = false; // a guest's own Ready (what it asked for)
    bool can_start = false;   // the host's Start is allowed
    bool have_state = false;  // a guest has the host's settings
    uint8_t max_players = 2;
    char title[kLobbyText] = {}; // the game's name
    char status[48] = {};        // Room: what's being waited for; Joining: whose room
    uint8_t room_count = 0;
    LobbyRoomRow rooms[kLobbyMaxRooms] = {};
    uint8_t player_count = 0;
    LobbyPlayerRow players[kLobbyMaxPlayers] = {};
    uint8_t setting_count = 0;
    LobbySettingRow settings[kLobbyMaxSettings] = {};
    uint8_t info_count = 0;
    char info[kLobbyMaxInfo][40] = {}; // Started: the start parameters, a line each
};

// What the player asked for this frame
struct LobbyCommand {
    enum Kind : uint8_t {
        None,
        Host,          // Menu
        Browse,        // Menu
        Join,          // Browse: rooms[index]
        ToggleReady,   // Room, guest
        Start,         // Room, host (only offered when can_start)
        ChangeSetting, // Room, host: settings[index] by delta
        BackToRoom,    // Started, host
        Leave,         // back to the Menu (closes the room or the list)
        Exit,          // B on the Menu: the app leaves the lobby
    };
    Kind kind = None;
    int8_t index = 0;
    int8_t delta = 0;
};

// The lobby's look: the system theme in unscii-8, which has lower case for
// player names
inline constexpr Theme kLobbyTheme = [] {
    Theme t;
    t.font = &kFontUnscii8;
    t.row_gap = 4;
    return t;
}();

class LobbyView {
  public:
    static constexpr Pixel kError = rgb_hex(0xff6b6b);
    static constexpr Pixel kGood = rgb_hex(0x2ed47a);
    static constexpr int kBarH = 16;
    static constexpr int kRowH = 14;

    int menu_focus = 0;
    int room_focus = 0;
    int setting_focus = 0;

    // Moves the focus and turns this frame's buttons into a command
    LobbyCommand update(const LobbySnapshot &s, const Keys &k) {
        if (notice_frames_ > 0) {
            --notice_frames_;
        }
        clamp(s);
        const int dy = k.down ? 1 : k.up ? -1 : 0;
        const int dx = k.right ? 1 : k.left ? -1 : 0;
        switch (s.page) {
        case LobbyPage::Menu:
            if (dy) {
                menu_focus = (menu_focus + 1) % 2;
            }
            if (k.a) {
                return {menu_focus == 0 ? LobbyCommand::Host : LobbyCommand::Browse};
            }
            if (k.b) {
                return {LobbyCommand::Exit};
            }
            break;
        case LobbyPage::Browse:
            if (dy && s.room_count > 0) {
                room_focus = std::clamp(room_focus + dy, 0, s.room_count - 1);
            }
            if (k.a && room_focus < s.room_count && s.rooms[room_focus].joinable) {
                return {LobbyCommand::Join, static_cast<int8_t>(room_focus)};
            }
            if (k.b) {
                return {LobbyCommand::Leave};
            }
            break;
        case LobbyPage::Joining:
            if (k.b) {
                return {LobbyCommand::Leave};
            }
            break;
        case LobbyPage::Room:
            if (s.is_host) {
                if (dy && s.setting_count > 0) {
                    setting_focus = (setting_focus + s.setting_count + dy) % s.setting_count;
                }
                if (dx && setting_focus < s.setting_count) {
                    return {LobbyCommand::ChangeSetting, static_cast<int8_t>(setting_focus),
                            static_cast<int8_t>(dx)};
                }
                if (k.a && s.can_start) {
                    return {LobbyCommand::Start};
                }
            } else if (k.a && s.have_state) {
                return {LobbyCommand::ToggleReady};
            }
            if (k.b) {
                return {LobbyCommand::Leave};
            }
            break;
        case LobbyPage::Started:
            if (k.a && s.is_host) {
                return {LobbyCommand::BackToRoom};
            }
            if (k.b) {
                return {LobbyCommand::Leave};
            }
            break;
        }
        return {};
    }

    // The hint bar's hints for this page (at most 4); returns how many
    int hints(const LobbySnapshot &s, Hint out[4]) const {
        int n = 0;
        auto add = [&](const char *b, const char *l) { out[n++] = Hint{b, l}; };
        switch (s.page) {
        case LobbyPage::Menu:
            add("A", menu_focus == 0 ? "Host" : "Find rooms");
            add("B", "Back");
            break;
        case LobbyPage::Browse:
            if (room_focus < s.room_count && s.rooms[room_focus].joinable) {
                add("A", "Join");
            }
            add("B", "Back");
            break;
        case LobbyPage::Joining:
            add("B", "Cancel");
            break;
        case LobbyPage::Room:
            if (s.is_host) {
                if (s.setting_count > 0) {
                    add("<>", "Change");
                }
                if (s.can_start) {
                    add("A", "Start");
                }
            } else if (s.have_state) {
                add("A", s.local_ready ? "Not ready" : "Ready");
            }
            add("B", "Leave");
            break;
        case LobbyPage::Started:
            if (s.is_host) {
                add("A", "Back to room");
            }
            add("B", "Leave");
            break;
        }
        return n;
    }

    // A line in a box at the top for a few seconds (frames of update())
    void notice(const char *text, bool error = false, int frames = 150) {
        std::strncpy(notice_, text, sizeof(notice_) - 1);
        notice_[sizeof(notice_) - 1] = 0;
        notice_error_ = error;
        notice_frames_ = frames;
    }
    bool notice_showing() const { return notice_frames_ > 0; }
    const char *notice_text() const { return notice_frames_ > 0 ? notice_ : ""; }

    // Draws the whole lobby in r: a title strip, the page, the hint bar
    void draw(Surface &sf, Rect r, const LobbySnapshot &s, const Theme &t = kLobbyTheme) const {
        sf.fill(r, rgb_hex(0x101216));
        // Title strip
        const Rect top{r.x, r.y, r.w, kBarH + 2};
        sf.fill(top, t.panel);
        sf.hline(top.x, top.right() - 1, top.bottom() - 1, t.line);
        const int ty = top.y + (top.h - t.font->height) / 2;
        text(sf, t, s.title[0] ? s.title : "Link play", r.x + 6, ty, t.text, 1);
        const char *tag = page_tag(s);
        text(sf, t, tag, r.right() - 6 - text_width(*t.font, tag), ty, t.accent, 1);
        // Hint bar
        Hint h[4];
        const int n = hints(s, h);
        hint_bar(sf, Rect{r.x, r.bottom() - kBarH, r.w, kBarH}, {h, static_cast<size_t>(n)}, t);
        // The page
        const Rect body{r.x + 8, top.bottom() + 6, r.w - 16, r.h - top.h - kBarH - 10};
        switch (s.page) {
        case LobbyPage::Menu:
            draw_menu(sf, body, t);
            break;
        case LobbyPage::Browse:
            draw_browse(sf, body, s, t);
            break;
        case LobbyPage::Joining:
            text_centered(sf, t, "Joining...", body.x, body.w, body.y + body.h / 2 - 12, t.text, 1);
            text_centered(sf, t, s.status, body.x, body.w, body.y + body.h / 2 + 2, t.dim, 1);
            break;
        case LobbyPage::Room:
            draw_room(sf, body, s, t);
            break;
        case LobbyPage::Started:
            draw_started(sf, body, s, t);
            break;
        }
        if (notice_frames_ > 0) {
            const int w = std::min(body.w, text_width(*t.font, notice_) + 16);
            const Rect nb{r.x + (r.w - w) / 2, top.bottom() + 4, w, 16};
            sf.fill(nb.grow(1), notice_error_ ? kError : t.accent);
            sf.fill(nb, t.panel);
            text_centered(sf, t, notice_, nb.x, nb.w, nb.y + 4, notice_error_ ? kError : t.text, 1);
        }
    }

  private:
    void clamp(const LobbySnapshot &s) {
        room_focus = std::clamp(room_focus, 0, std::max(0, s.room_count - 1));
        setting_focus = std::clamp(setting_focus, 0, std::max(0, s.setting_count - 1));
    }

    static const char *page_tag(const LobbySnapshot &s) {
        switch (s.page) {
        case LobbyPage::Menu:
            return "LINK PLAY";
        case LobbyPage::Browse:
            return "ROOMS";
        case LobbyPage::Joining:
            return "JOINING";
        case LobbyPage::Room:
            return s.is_host ? "HOSTING" : "IN A ROOM";
        case LobbyPage::Started:
            return "STARTED";
        }
        return "";
    }

    static void section(Surface &sf, const Theme &t, const char *label, int x, int y) {
        text(sf, t, label, x, y, t.dim, 1);
    }

    static void row_box(Surface &sf, Rect r, const Theme &t, bool focused) {
        sf.fill(r, focused ? rgb_hex(0x2a2e35) : t.panel);
        if (focused) {
            sf.fill(Rect{r.x, r.y, 3, r.h}, t.accent);
        }
    }

    void draw_menu(Surface &sf, Rect b, const Theme &t) const {
        static constexpr const char *kRows[2] = {"Host a room", "Find rooms"};
        const int y0 = b.y + b.h / 2 - kRowH - 4;
        for (int i = 0; i < 2; i++) {
            const Rect row{b.x + b.w / 4, y0 + i * (kRowH + 8), b.w / 2, kRowH + 4};
            row_box(sf, row, t, i == menu_focus);
            text_centered(sf, t, kRows[i], row.x, row.w, row.y + 6,
                          i == menu_focus ? t.accent : t.text, 1);
        }
    }

    void draw_browse(Surface &sf, Rect b, const LobbySnapshot &s, const Theme &t) const {
        section(sf, t, "ROOMS", b.x, b.y);
        if (s.room_count == 0) {
            text(sf, t, "Looking for rooms...", b.x + 4, b.y + 18, t.dim, 1);
            return;
        }
        const int visible = std::max(1, (b.h - 14) / (kRowH + 2));
        const int first = std::clamp(room_focus - visible + 1, 0, std::max(0, s.room_count - 1));
        for (int i = first; i < s.room_count && i - first < visible; i++) {
            const LobbyRoomRow &room = s.rooms[i];
            const Rect row{b.x, b.y + 12 + (i - first) * (kRowH + 2), b.w, kRowH};
            const bool on = i == room_focus;
            row_box(sf, row, t, on);
            text(sf, t, room.name, row.x + 8, row.y + 3,
                 room.joinable ? (on ? t.accent : t.text) : t.dim, 1);
            text(sf, t, room.detail, row.right() - 6 - text_width(*t.font, room.detail), row.y + 3,
                 room.joinable ? t.dim : kError, 1);
        }
    }

    void draw_room(Surface &sf, Rect b, const LobbySnapshot &s, const Theme &t) const {
        char head[24] = "PLAYERS ";
        size_t n = std::strlen(head);
        head[n++] = static_cast<char>('0' + s.player_count);
        head[n++] = '/';
        head[n++] = static_cast<char>('0' + s.max_players);
        head[n] = 0;
        section(sf, t, head, b.x, b.y);
        int y = b.y + 12;
        for (int i = 0; i < s.player_count; i++) {
            const LobbyPlayerRow &p = s.players[i];
            const Rect row{b.x, y, b.w, kRowH};
            row_box(sf, row, t, false);
            // Ready mark: a filled square when ready, an outline when not
            const Rect mark{row.x + 5, row.y + 3, 8, 8};
            if (p.ready) {
                sf.fill(mark, kGood);
            } else {
                sf.frame(mark, t.dim);
            }
            text(sf, t, p.name, row.x + 18, row.y + 3, p.local ? t.accent : t.text, 1);
            const char *tag = p.lagging ? "LAG" : p.host ? "HOST" : p.ready ? "READY" : "NOT READY";
            const Pixel tc = p.lagging ? kError : p.ready ? kGood : t.dim;
            int x = row.right() - 6 - text_width(*t.font, tag);
            text(sf, t, tag, x, row.y + 3, tc, 1);
            if (p.local) {
                x -= text_width(*t.font, "YOU ") + 2;
                text(sf, t, "YOU", x, row.y + 3, t.dim, 1);
            }
            y += kRowH + 2;
        }
        if (s.setting_count > 0) {
            y += 4;
            section(sf, t, "SETTINGS", b.x, y);
            y += 12;
            for (int i = 0; i < s.setting_count; i++) {
                const LobbySettingRow &st = s.settings[i];
                const bool on = s.is_host && i == setting_focus;
                const Rect row{b.x, y, b.w, kRowH};
                row_box(sf, row, t, on);
                text(sf, t, st.name, row.x + 8, row.y + 3, on ? t.accent : t.text, 1);
                char value[kLobbyText + 4];
                const char *v = st.value;
                if (on) {
                    value[0] = '<';
                    value[1] = ' ';
                    std::strncpy(value + 2, st.value, kLobbyText - 1);
                    value[kLobbyText + 1] = 0;
                    std::strcat(value, " >");
                    v = value;
                }
                text(sf, t, v, row.right() - 6 - text_width(*t.font, v), row.y + 3,
                     on ? t.accent : t.text, 1);
                y += kRowH + 2;
            }
        }
        const Pixel sc = s.can_start ? kGood : t.dim;
        text(sf, t, s.status, b.x, std::max(y + 4, b.bottom() - t.font->height), sc, 1);
    }

    void draw_started(Surface &sf, Rect b, const LobbySnapshot &s, const Theme &t) const {
        section(sf, t, "START", b.x, b.y);
        for (int i = 0; i < s.info_count; i++) {
            text(sf, t, s.info[i], b.x + 4, b.y + 14 + i * 12, i == 0 ? t.text : t.dim, 1);
        }
    }

    char notice_[40] = {};
    bool notice_error_ = false;
    int notice_frames_ = 0;
};

} // namespace gfx::ui
