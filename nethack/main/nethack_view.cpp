// The terminal view (nethack_view.hpp).

#include "nethack_view.hpp"

#include <algorithm>

#include "gfx/font_unscii8.hpp"
#include "nethack_first_person.hpp"
#include "nethack_tiles.hpp"

namespace nethack {
namespace {

const gfx::Font &kFont = gfx::kFontUnscii8;
constexpr int kCell = 8;  // the font's cell, square
constexpr int kPitch = 9; // a menu's or text window's line

constexpr gfx::Pixel kBackground = gfx::rgb(0, 0, 0);
constexpr gfx::Pixel kText = gfx::rgb(200, 200, 200);
constexpr gfx::Pixel kBright = gfx::rgb(255, 255, 255);
constexpr gfx::Pixel kDim = gfx::rgb(110, 110, 120);
constexpr gfx::Pixel kAccent = gfx::rgb(255, 200, 60);
constexpr gfx::Pixel kPanel = gfx::rgb(16, 18, 34);
constexpr gfx::Pixel kPanelEdge = gfx::rgb(70, 80, 140);
constexpr gfx::Pixel kCursor = gfx::rgb(40, 60, 120);

// NetHack's colors (CLR_*). Black is drawn dark gray (WC2_DARKGRAY), so a
// black monster shows on the black screen.
constexpr gfx::Pixel kColors[16] = {
    gfx::rgb(85, 85, 95),    // black
    gfx::rgb(205, 40, 40),   // red
    gfx::rgb(40, 170, 40),   // green
    gfx::rgb(170, 110, 30),  // brown
    gfx::rgb(60, 80, 220),   // blue
    gfx::rgb(170, 50, 170),  // magenta
    gfx::rgb(40, 170, 170),  // cyan
    gfx::rgb(190, 190, 190), // gray
    gfx::rgb(190, 190, 190), // no color
    gfx::rgb(255, 140, 30),  // orange
    gfx::rgb(90, 255, 90),   // bright green
    gfx::rgb(255, 255, 70),  // yellow
    gfx::rgb(110, 140, 255), // bright blue
    gfx::rgb(255, 100, 255), // bright magenta
    gfx::rgb(100, 255, 255), // bright cyan
    gfx::rgb(255, 255, 255), // white
};

// What the hero remembers but doesn't see: 18/32 of its color (blend's
// amount is out of 32)
gfx::Pixel dimmed(gfx::Pixel p) { return gfx::blend(kBackground, p, 18); }

// Text clipped to `cols` characters
void text(gfx::Surface &s, const std::string &t, int x, int y, gfx::Pixel c, int cols = 1000) {
    s.text(kFont, std::string_view(t).substr(0, static_cast<size_t>(std::max(cols, 0))), x, y, c);
}

// Words wrapped to `cols` characters a line
std::vector<std::string> wrap(const std::string &t, int cols) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < t.size()) {
        size_t end = t.find(' ', i);
        if (end == std::string::npos) {
            end = t.size();
        }
        std::string word = t.substr(i, end - i);
        if (!line.empty() && static_cast<int>(line.size() + 1 + word.size()) > cols) {
            lines.push_back(line);
            line.clear();
        }
        while (static_cast<int>(word.size()) > cols) {
            lines.push_back(word.substr(0, cols));
            word = word.substr(cols);
        }
        line += (line.empty() ? "" : " ") + word;
        i = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

std::vector<MenuItem> text_lines(const Window &w, int width) {
    const int cols = std::max(width / kCell - 1, 8);
    std::vector<MenuItem> out;
    for (const MenuItem &item : w.items) {
        if (static_cast<int>(item.text.size()) <= cols) {
            out.push_back(item);
            continue;
        }
        // Wrapped lines keep the line's indent
        const size_t first = item.text.find_first_not_of(' ');
        const int indent = std::min(static_cast<int>(first), cols / 2);
        const std::string pad(static_cast<size_t>(indent), ' ');
        for (const std::string &part : wrap(item.text.substr(first), cols - indent)) {
            MenuItem line = item;
            line.text = pad + part;
            out.push_back(std::move(line));
        }
    }
    return out;
}

namespace {

struct Layout {
    int cols;      // characters across
    int msg_y;     // the message lines
    int msg_lines; //
    int map_y;     // the map's top
    int map_cols;  // map columns shown
    int status_y;  // the status lines
    int hint_y;    // the controls line
};

Layout layout(const gfx::Surface &s) {
    Layout l{};
    l.cols = s.w / kCell;
    l.msg_y = 1;
    l.msg_lines = 2;
    l.map_y = l.msg_y + l.msg_lines * kCell + 2;
    l.map_cols = std::min(l.cols, kCols - 1);
    l.status_y = l.map_y + kRows * kCell + 3;
    l.hint_y = s.h - kCell - 1;
    return l;
}

int scroll_x(const Model &m, int map_cols) {
    // Columns 1..79; the hero (or the cursor, choosing a position) kept in
    // the middle where it can be
    const int first = 1, last = kCols - map_cols;
    const int x = m.getpos ? m.cursor_x : m.hero_x;
    return std::clamp(x - map_cols / 2, first, std::max(first, last));
}

void draw_map(gfx::Surface &s, const Model &m, const Layout &l) {
    const int x0 = scroll_x(m, l.map_cols);
    const int left = (s.w - l.map_cols * kCell) / 2;
    for (int y = 0; y < kRows; y++) {
        for (int i = 0; i < l.map_cols; i++) {
            const Cell &c = m.cell(x0 + i, y);
            if (c.ch == ' ') {
                continue;
            }
            const int px = left + i * kCell, py = l.map_y + y * kCell;
            gfx::Pixel color = kColors[c.color & 15];
            if (c.flags & NHV_CELL_REMEMBERED) {
                color = dimmed(color);
            }
            if (c.flags & (NHV_CELL_PET | NHV_CELL_INVERSE)) {
                s.fill({px, py, kCell, kCell}, dimmed(color));
                color = kBright;
            }
            char ch[2] = {c.ch, 0};
            s.text(kFont, ch, px, py, color);
        }
    }
    // The hero's cell outlined, so it's found at a glance
    if (m.hero_x >= x0 && m.hero_x < x0 + l.map_cols) {
        s.frame({left + (m.hero_x - x0) * kCell - 1, l.map_y + m.hero_y * kCell - 1, kCell + 2,
                 kCell + 2},
                gfx::rgb(80, 80, 40));
    }
    // Choosing a position: the cursor
    if (m.getpos && m.cursor_x >= x0 && m.cursor_x < x0 + l.map_cols && m.cursor_y >= 0 &&
        m.cursor_y < kRows) {
        const gfx::Rect r{left + (m.cursor_x - x0) * kCell - 2, l.map_y + m.cursor_y * kCell - 2,
                          kCell + 4, kCell + 4};
        s.frame(r, kAccent);
        s.frame({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, kBright);
    }
}

// The pets' mark in the tile views (the tiles don't show which are tame)
void draw_heart(gfx::Surface &s, int x, int y) {
    static const char *const kHeart[] = {".x.x.", "xxxxx", ".xxx.", "..x.."};
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 5; c++) {
            if (kHeart[r][c] == 'x') {
                s.set(x + c, y + r, gfx::rgb(255, 60, 90));
            }
        }
    }
}

// The map in NetHack's tiles, 16 or 8 pixels a cell, scrolled to keep the
// hero (or the cursor, choosing a position) in the middle where it can be
void draw_tiles(gfx::Surface &s, const Model &m, const Layout &l, TileSet &tiles, bool small) {
    const int size = small ? TileSet::kSmall : TileSet::kSize;
    const int area_h = l.status_y - 2 - l.map_y;
    const int cols = std::min(s.w / size, kCols - 1);
    const int rows = std::min(area_h / size, kRows);
    const int fx = m.getpos ? m.cursor_x : m.hero_x;
    const int fy = m.getpos ? m.cursor_y : m.hero_y;
    const int x0 = std::clamp(fx - cols / 2, 1, std::max(1, kCols - cols));
    const int y0 = std::clamp(fy - rows / 2, 0, std::max(0, kRows - rows));
    const int left = (s.w - cols * size) / 2;
    const int top = l.map_y + (area_h - rows * size) / 2;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const Cell &cell = m.cell(x0 + c, y0 + r);
            const gfx::Pixel *px = tiles.tile(cell.tile, small);
            if (!px) {
                continue;
            }
            const int x = left + c * size, y = top + r * size;
            const bool dim = (cell.flags & NHV_CELL_REMEMBERED) != 0;
            for (int ty = 0; ty < size; ty++) {
                gfx::Pixel *out = s.row(y + ty) + x;
                const gfx::Pixel *in = px + ty * size;
                for (int tx = 0; tx < size; tx++) {
                    out[tx] = dim ? dimmed(in[tx]) : in[tx];
                }
            }
            if ((cell.flags & NHV_CELL_PET) && !small) {
                draw_heart(s, x + 1, y + 1);
            }
        }
    }
    const auto outline = [&](int cx, int cy, gfx::Pixel a, gfx::Pixel b) {
        if (cx < x0 || cx >= x0 + cols || cy < y0 || cy >= y0 + rows) {
            return;
        }
        const gfx::Rect rc{left + (cx - x0) * size - 1, top + (cy - y0) * size - 1, size + 2,
                           size + 2};
        s.frame(rc, a);
        if (b != a) {
            s.frame({rc.x - 1, rc.y - 1, rc.w + 2, rc.h + 2}, b);
        }
    };
    if (!small) {
        outline(m.hero_x, m.hero_y, gfx::rgb(120, 120, 60), gfx::rgb(120, 120, 60));
    }
    if (m.getpos) {
        outline(m.cursor_x, m.cursor_y, kBright, kAccent);
    }
}

void draw_messages(gfx::Surface &s, const Model &m, const Layout &l) {
    // The newest messages, wrapped, as many as fit
    std::vector<std::string> lines;
    const size_t shown = std::max<size_t>(std::min<size_t>(m.new_messages, 4), 1);
    for (size_t i = m.messages.size() > shown ? m.messages.size() - shown : 0;
         i < m.messages.size(); i++) {
        for (auto &line : wrap(m.messages[i], l.cols)) {
            lines.push_back(line);
        }
    }
    const size_t first =
        lines.size() > static_cast<size_t>(l.msg_lines) ? lines.size() - l.msg_lines : 0;
    const gfx::Pixel color = m.new_messages ? kBright : kDim;
    for (size_t i = first; i < lines.size(); i++) {
        text(s, lines[i], 0, l.msg_y + static_cast<int>(i - first) * kCell, color, l.cols);
    }
}

void draw_status(gfx::Surface &s, const Model &m, const Layout &l) {
    int y = l.status_y;
    for (const auto &line : m.status) {
        // NetHack pads with spaces; squeeze runs of them to fit a small screen
        std::string squeezed;
        for (char c : line) {
            if (c != ' ' || squeezed.empty() || squeezed.back() != ' ') {
                squeezed += c;
            }
        }
        for (auto &part : wrap(squeezed, l.cols)) {
            if (y + kCell > l.hint_y) {
                return;
            }
            text(s, part, 0, y, kText, l.cols);
            y += kCell;
        }
    }
}

void panel(gfx::Surface &s, gfx::Rect r) {
    s.fill(r, kPanel);
    s.frame(r, kPanelEdge);
}

void draw_hints(gfx::Surface &s, const Layout &l, const std::string &hints) {
    s.fill({0, l.hint_y - 1, s.w, kCell + 2}, kBackground);
    text(s, hints, 0, l.hint_y, kDim, l.cols);
}

// A menu or text window, filling the screen
void draw_window(gfx::Surface &s, const Model &m, const ViewState &v, const Layout &l) {
    const Window *found = m.window(m.request.window);
    if (!found) {
        return;
    }
    const Window &w = *found;
    const bool menu = m.request.ask == Ask::Menu;
    panel(s, {0, 0, s.w, s.h});
    int y = 3;
    if (!w.prompt.empty()) {
        text(s, w.prompt, 4, y, kAccent, l.cols - 1);
        y += kPitch + 2;
    }
    const int rows = page_lines(s.h) - (w.prompt.empty() ? 0 : 1);
    // A text window's long lines are wrapped; a menu's are cut
    const std::vector<MenuItem> wrapped = menu ? std::vector<MenuItem>{} : text_lines(w, s.w);
    const auto &items = menu ? w.items : wrapped;
    const int count = static_cast<int>(items.size());
    for (int i = v.top; i < std::min(count, v.top + rows); i++) {
        const MenuItem &item = items[i];
        if (menu && i == v.cursor) {
            s.fill({1, y - 1, s.w - 2, kPitch}, kCursor);
        }
        std::string line;
        gfx::Pixel color = kText;
        if (item.index) {
            const bool marked = i < static_cast<int>(v.marks.size()) && v.marks[i];
            if (menu && m.request.pick == NHV_PICK_ANY) {
                line = marked ? "[x] " : "[ ] ";
            }
            if (item.accelerator) {
                line += std::string(1, item.accelerator) + " - ";
            }
        } else if (item.attr) {
            color = kAccent; // a heading
        }
        if (item.color >= 0 && item.color < 16 && item.index) {
            color = kColors[item.color];
        }
        line += item.text;
        text(s, line, 4, y, color, l.cols - 1);
        y += kPitch;
    }
    std::string hints;
    if (!menu) {
        hints = count > rows ? "Up/Down: scroll  A/B: close" : "A/B: close";
    } else if (m.request.pick == NHV_PICK_ANY) {
        hints = "A: mark  Start: done  B: cancel";
    } else {
        hints = "A: choose  B: cancel  L/R: page";
    }
    if (count > rows) {
        hints += "  " + std::to_string(v.top / std::max(rows, 1) + 1) + "/" +
                 std::to_string((count + rows - 1) / std::max(rows, 1));
    }
    draw_hints(s, l, hints);
}

void draw_yes_no(gfx::Surface &s, const Model &m, const ViewState &v, const Layout &l) {
    const Request &r = m.request;
    auto lines = wrap(r.query, l.cols - 2);
    const int choices = static_cast<int>(r.choices.size());
    const int h = static_cast<int>(lines.size()) * kCell + (choices ? kCell + 6 : 0) + 8;
    panel(s, {0, 0, s.w, h});
    int y = 4;
    for (auto &line : lines) {
        text(s, line, 8, y, kBright, l.cols - 2);
        y += kCell;
    }
    if (choices) {
        y += 3;
        int x = 8;
        for (int i = 0; i < choices; i++) {
            const char ch = r.choices[i];
            if (ch == kEsc) {
                break; // the rest are synonyms NetHack accepts
            }
            const gfx::Rect chip{x - 2, y - 2, 3 * kCell + 4, kCell + 4};
            if (i == v.choice) {
                s.fill(chip, kCursor);
            }
            std::string label(1, ch);
            if (ch == r.def) {
                label = "(" + label + ")";
            }
            text(s, label, x + (label.size() == 1 ? kCell : 0), y, i == v.choice ? kBright : kText);
            x += 3 * kCell + 8;
        }
    }
    draw_hints(s, l,
               choices ? "Left/Right: choose  A: answer  B: cancel"
                       : "D-pad: direction  A: here / list  B: cancel");
}

void draw_more(gfx::Surface &s, const Model &m, const Layout &l) {
    // The messages since the last key, as many as fit
    std::vector<std::string> lines;
    const size_t n = std::min(m.new_messages, m.messages.size());
    for (size_t i = m.messages.size() - n; i < m.messages.size(); i++) {
        for (auto &line : wrap(m.messages[i], l.cols - 2)) {
            lines.push_back(line);
        }
    }
    const int max_lines = (s.h - 30) / kCell;
    if (static_cast<int>(lines.size()) > max_lines) {
        lines.erase(lines.begin(), lines.end() - max_lines);
    }
    const int h = static_cast<int>(lines.size()) * kCell + kCell + 12;
    panel(s, {0, 0, s.w, h});
    int y = 4;
    for (auto &line : lines) {
        text(s, line, 8, y, kBright, l.cols - 2);
        y += kCell;
    }
    text(s, "--More--", 8, y + 2, kAccent);
    draw_hints(s, l, "A: continue");
}

} // namespace

int page_lines(int h) { return std::max((h - 3 - kCell - 4) / kPitch, 1); }

void draw(gfx::Surface &s, const Model &m, const ViewState &v) {
    const Layout l = layout(s);
    s.clear(kBackground);
    switch (m.request.ask) {
    case Ask::Menu:
    case Ask::Text:
        draw_window(s, m, v, l);
        return;
    default:
        break;
    }
    const bool tiles = v.map_view != MapView::Terminal && v.tiles && v.tiles->is_open();
    if (tiles && v.map_view == MapView::FirstPerson && v.first_person && !m.getpos) {
        const int height = l.status_y - 2 - l.map_y;
        v.first_person->draw(s, l.map_y, height, m, *v.tiles, v.camera, v.facing);
    } else if (tiles) {
        draw_tiles(s, m, l, *v.tiles, v.map_view == MapView::SmallTiles);
    } else {
        draw_map(s, m, l);
    }
    draw_messages(s, m, l);
    draw_status(s, m, l);
    switch (m.request.ask) {
    case Ask::YesNo:
        draw_yes_no(s, m, v, l);
        break;
    case Ask::More:
        draw_more(s, m, l);
        break;
    default:
        draw_hints(s, l, !v.note.empty() ? v.note : !v.hints.empty() ? v.hints : "");
        break;
    }
}

void draw_list(gfx::Surface &s, const std::string &title, const std::vector<std::string> &items,
               int cursor, int top, const std::string &hints) {
    const Layout l = layout(s);
    s.clear(kBackground);
    panel(s, {0, 0, s.w, s.h});
    text(s, title, 4, 3, kAccent, l.cols - 1);
    int y = 3 + kPitch + 2;
    const int rows = page_lines(s.h) - 1;
    for (int i = top; i < std::min(static_cast<int>(items.size()), top + rows); i++) {
        if (i == cursor) {
            s.fill({1, y - 1, s.w - 2, kPitch}, kCursor);
        }
        text(s, items[i], 4, y, kText, l.cols - 1);
        y += kPitch;
    }
    draw_hints(s, l, hints);
}

void draw_message(gfx::Surface &s, const std::string &message) {
    s.clear(kBackground);
    const int cols = s.w / kCell - 2;
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= message.size()) {
        size_t end = message.find('\n', start);
        if (end == std::string::npos) {
            end = message.size();
        }
        auto wrapped = wrap(message.substr(start, end - start), cols);
        if (wrapped.empty()) {
            lines.emplace_back();
        }
        lines.insert(lines.end(), wrapped.begin(), wrapped.end());
        start = end + 1;
    }
    int y = (s.h - static_cast<int>(lines.size()) * kPitch) / 2;
    for (size_t i = 0; i < lines.size(); i++) {
        const int w = static_cast<int>(lines[i].size()) * kCell;
        text(s, lines[i], (s.w - w) / 2, y, i == 0 ? kAccent : kText);
        y += kPitch;
    }
}

} // namespace nethack
