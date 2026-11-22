#include "wfc/sockets.hpp"

#include <algorithm>

namespace wfc {

int SocketTiles::add(std::string_view name, const std::array<std::string, 4> &sockets, float weight,
                     int rotations) {
    const int base = static_cast<int>(names_.size());
    const int first = size();
    names_.emplace_back(name);
    for (int r = 0; r < rotations; ++r) {
        Variant v{base, r, {}, weight};
        // Turning clockwise: what faced d now faces d + r
        for (int d = 0; d < 4; ++d) {
            v.sockets[(d + r) & 3] = sockets[d];
        }
        variants_.push_back(std::move(v));
    }
    return first;
}

int SocketTiles::find(std::string_view name, int rotation) const {
    for (int i = 0; i < size(); ++i) {
        if (variants_[i].rotation == rotation && names_[variants_[i].base] == name) {
            return i;
        }
    }
    return -1;
}

bool SocketTiles::fits(int a, Dir d, int b) const {
    const std::string &mine = variants_[a].sockets[d];
    const std::string &theirs = variants_[b].sockets[opposite(d)];
    return mine.size() == theirs.size() && std::equal(mine.begin(), mine.end(), theirs.rbegin());
}

Rules SocketTiles::rules() const {
    Rules rules(size());
    for (int a = 0; a < size(); ++a) {
        rules.set_weight(a, variants_[a].weight);
        for (int b = 0; b < size(); ++b) {
            // East and south cover each pair; allow() adds the reverse
            for (Dir d : {East, South}) {
                if (fits(a, d, b)) {
                    rules.allow(a, d, b);
                }
            }
        }
    }
    return rules;
}

} // namespace wfc
