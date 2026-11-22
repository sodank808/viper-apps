#pragma once

// Deterministic lockstep for link play: two to four units run copies of the
// same deterministic simulation and exchange only their inputs. Tick T runs
// on every unit with the same inputs for every player, or doesn't run yet.
// Design: docs/design/multiplayer.md ("InputSync"); usage:
// components/game/README.md.
//
//   - Each unit schedules the input it reads while tick T is due for tick
//     T + D (the input delay, agreed at start). Ticks 0 to D - 1 run with
//     every input zero.
//   - Every frame each unit sends each other player one small packet with
//     its inputs that player hasn't acknowledged (so a lost packet costs
//     nothing: the next one repeats them), the first tick of that player's
//     input it still lacks (the ack), its own next tick, the schedule shift
//     and its latest state hash.
//   - Tick N is due at start_us + shift + N * tick_us on the session clock,
//     the same instant on every unit. advance() runs the ticks that are due
//     and whose inputs have all arrived (at most kMaxTicksPerFrame in one
//     frame, to catch up), and returns how long until the next is due, so
//     frames land on the schedule and the units' crystals don't drift apart.
//     After a long stall the lowest player slot (the host) moves the schedule
//     back (the shift) and the others follow it, so nobody runs a long burst
//     of catch-up ticks.
//   - A due tick missing an input is a stall: the frame steps nothing and
//     says which players it waits for; after kLagUs on one player it says
//     that player is lagging. A player leaving the session, quitting or the
//     session ending ends the match (Ended). Keeping on with a missing
//     player's input held neutral needs the remaining players to agree on
//     the tick it stops at, and isn't built.
//   - The game reports a hash of its state after each hash_due tick (one in
//     kHashInterval); hashes travel in the packets and a mismatch ends the
//     match as Desynced, naming the tick.
//
// Header-only and platform-free: no clock, radio or allocation of its own. It
// talks to the link through a SyncPort (the session clock, the roster, and
// sending a packet to a slot); received packets are handed to receive(). The
// OS's adapter is LinkSyncPort (components/system, over LinkService); tests
// use one over a Session and Messenger. Fixed-size buffers only: nothing is
// allocated, and nothing blocks. Single-threaded.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace game {

inline constexpr uint8_t kSyncMaxPlayers = 4;
/// Largest input record per player per tick, in bytes
inline constexpr uint8_t kSyncMaxInputSize = 8;
inline constexpr uint8_t kSyncNoSlot = 0xFF;

/// FNV-1a, 32 bits: for the match id, and handy for hashing game state.
struct Fnv1a {
    uint32_t value = 2166136261u;
    Fnv1a &add(const void *data, size_t size) {
        const auto *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; ++i) {
            value = (value ^ p[i]) * 16777619u;
        }
        return *this;
    }
    template <typename T> Fnv1a &add(const T &v) { return add(&v, sizeof(v)); }
};

/// What InputSync needs from the link. LinkSyncPort (components/system)
/// implements it over LinkService; tests over a Session and Messenger.
class SyncPort {
  public:
    /// The session clock (the host's clock), in microseconds
    virtual int64_t now_us() const = 0;
    /// This unit's slot, or kSyncNoSlot when it isn't in a session
    virtual uint8_t local_slot() const = 0;
    /// The slot is a member of the session now
    virtual bool present(uint8_t slot) const = 0;
    /// Send one packet to one member, unreliably (InputSync::kMessageType).
    /// False if it couldn't be sent; InputSync sends again next frame anyway.
    virtual bool send(uint8_t to_slot, std::span<const uint8_t> packet) = 0;

  protected:
    ~SyncPort() = default;
};

/// What every player must agree on before tick 0. The host fills it in and
/// sends it reliably (the lobby's Start message carries it, encoded); every
/// unit, the host too, passes the same values to InputSync::start().
struct SyncStart {
    static constexpr uint8_t kVersion = 1;
    static constexpr size_t kEncodedSize = 20;

    uint32_t seed = 0;    ///< the game's: seed its game::Rng with it
    int64_t start_us = 0; ///< tick 0 is due at this session-clock time
    uint32_t tick_us = 16'667;
    uint8_t input_delay = 2; ///< ticks; InputSync::choose_delay()
    uint8_t input_size = 2;  ///< bytes per player per tick, 1 to kSyncMaxInputSize
    uint8_t players = 0b11;  ///< a bit per playing slot (slot 0 is bit 0)

    bool valid() const;
    /// Players in the match
    int player_count() const;
    /// The lowest playing slot: it keeps the schedule (normally the host)
    uint8_t timekeeper() const;
    /// Little-endian, kEncodedSize bytes; 0 if `out` is too small
    size_t encode(std::span<uint8_t> out) const;
    /// False if too short, another version or not valid()
    static bool decode(std::span<const uint8_t> in, SyncStart &out);
    /// A hash of all of it: packets carry it, so a unit that started with
    /// other values (or a previous match) is ignored
    uint32_t match_id() const;
};

enum class SyncStatus : uint8_t {
    Idle,     ///< not started
    Starting, ///< started, before tick 0 is due
    Running,  ///< stepped every tick that was due (possibly none this frame)
    Waiting,  ///< a due tick lacks a player's input (waiting_for)
    Lagging,  ///< waited for a player for kLagUs or more (lagging; draw "Waiting for player")
    Ended,    ///< over (end, end_slot); nothing more steps
    Desynced, ///< state hashes differed (desync_tick, desync_slot); nothing more steps
};

enum class SyncEnd : uint8_t {
    None,
    PeerLeft,     ///< end_slot left the session (or timed out of it)
    PeerQuit,     ///< end_slot's InputSync stopped or ended
    SessionEnded, ///< this unit isn't in the session any more (the host went)
    Stopped,      ///< stop() was called here
};

const char *to_string(SyncStatus s);
const char *to_string(SyncEnd e);

/// One tick to run, with every player's input for it.
struct SyncTick {
    uint32_t number = 0;
    /// Report the state hash after this tick: InputSync::report_hash(number, ...)
    bool hash_due = false;
    uint8_t input_size = 0;
    std::array<std::array<uint8_t, kSyncMaxInputSize>, kSyncMaxPlayers> inputs{};

    /// Slot's input (all zero for a slot that isn't playing)
    std::span<const uint8_t> input(uint8_t slot) const { return {inputs[slot].data(), input_size}; }
    /// The first two bytes, little-endian: the default 2-byte pad record
    uint16_t pad(uint8_t slot) const {
        return static_cast<uint16_t>(inputs[slot][0] | (inputs[slot][1] << 8));
    }
};

/// What advance() returns: valid until the next advance().
struct SyncFrame {
    static constexpr int kMaxTicks = 2;

    SyncStatus status = SyncStatus::Idle;
    uint8_t ticks = 0; ///< run tick[0] .. tick[ticks - 1], in order
    std::array<SyncTick, kMaxTicks> tick{};
    uint8_t waiting_for = 0; ///< slot mask: whose input a due tick lacks
    uint8_t lagging = 0;     ///< slot mask: waited on for kLagUs or more
    /// Until the next frame should run, on the session clock
    int64_t interval_us = 16'667;
    SyncEnd end = SyncEnd::None;
    uint8_t end_slot = kSyncNoSlot;
    uint32_t desync_tick = 0;
    uint8_t desync_slot = kSyncNoSlot;

    /// interval_us rounded up, for onLoop()'s return value
    uint32_t interval_ms() const { return static_cast<uint32_t>((interval_us + 999) / 1000); }
    bool over() const { return status == SyncStatus::Ended || status == SyncStatus::Desynced; }
};

/// Counters since start().
struct SyncStats {
    uint32_t ticks = 0;          ///< ticks handed out to run
    uint32_t frames = 0;         ///< advance() calls after start
    uint32_t stall_frames = 0;   ///< frames where a due tick waited for an input
    uint32_t catchup_frames = 0; ///< frames that ran more than one tick
    uint32_t shifts = 0;         ///< times the schedule moved back after a stall
    int64_t shift_us = 0;        ///< how far, in all
    uint32_t packets_sent = 0;
    uint32_t send_failed = 0;
    uint32_t inputs_sent = 0; ///< input records carried, redundant ones included
    uint32_t max_inputs_in_packet = 0;
    uint32_t packets_received = 0; ///< accepted
    uint32_t inputs_received = 0;  ///< new input records stored
    uint32_t inputs_redundant = 0; ///< records already had, or already run
    uint32_t ignored = 0;          ///< not started, another match, not a player, ourselves
    uint32_t wrong_version = 0;
    uint32_t malformed = 0;
    uint32_t hashes_compared = 0;
};

class InputSync {
  public:
    /// Messages of this type carry InputSync packets (unreliable). Reserved
    /// for system layers (link::Messenger::kFirstReservedType is 0xE0).
    static constexpr uint8_t kMessageType = 0xE1;
    static constexpr uint8_t kPacketVersion = 1;

    static constexpr uint32_t kDefaultTickUs = 16'667;
    /// Input delay bounds. 1 isn't offered until Phase 0's game-loop test
    /// runs at 24 Mbps (docs/design/multiplayer.md, Measurements).
    static constexpr uint8_t kMinDelay = 2;
    static constexpr uint8_t kMaxDelay = 6;
    /// Added to the one-way delay estimate in choose_delay(): session clock
    /// error, frames landing up to a millisecond late, a slow frame
    static constexpr int64_t kDelayMarginUs = 4'000;
    /// A tick in this many is hash_due (ticks 59, 119, ...)
    static constexpr uint32_t kHashInterval = 60;
    /// Waiting on one player this long makes it lagging
    static constexpr int64_t kLagUs = 250'000;
    /// Frame interval while waiting for an input: come back soon
    static constexpr int64_t kRetryUs = 4'000;
    /// Shortest frame interval returned (catching up)
    static constexpr int64_t kMinIntervalUs = 1'000;
    /// The timekeeper moves the schedule back rather than fall further than
    /// this many ticks behind it (a tenth of a second at 60 Hz)
    static constexpr uint32_t kMaxBehindTicks = 6;
    /// Inputs kept per player: covers 2 * kMaxDelay + 1 unacknowledged
    /// local ticks, and remote ones up to that far ahead
    static constexpr uint32_t kRing = 32;
    /// Most input records in one packet. Unacknowledged local inputs never
    /// exceed 2 * D + 1 (13), so this only bounds a misbehaving peer's ack.
    static constexpr uint32_t kMaxInputsPerPacket = 16;
    /// Packet layout, little-endian:
    ///   u8 version, u8 flags (kFlagHash, kFlagQuit), u32 match id,
    ///   u32 sender's next tick, u32 ack (first tick of the addressee's input
    ///   the sender lacks), i64 schedule shift (us), u32 first input tick,
    ///   u8 count, count * input_size bytes, then if kFlagHash: u32 hash
    ///   tick, u32 hash.
    static constexpr size_t kHeaderSize = 27;
    static constexpr size_t kMaxPacket =
        kHeaderSize + kMaxInputsPerPacket * kSyncMaxInputSize + 8; // 163
    static constexpr uint8_t kFlagHash = 1;
    static constexpr uint8_t kFlagQuit = 2;

    /// The input delay for a link with this round trip and jitter (the
    /// session's smoothed values to the worst peer): enough ticks to cover
    /// half the round trip, twice the jitter and kDelayMarginUs, clamped to
    /// kMinDelay..kMaxDelay. Frames run on the shared schedule, so the input
    /// for T + D, sent while T runs, has D ticks to arrive.
    static uint8_t choose_delay(int64_t rtt_us, int64_t jitter_us,
                                uint32_t tick_us = kDefaultTickUs) {
        if (tick_us == 0) {
            return kMaxDelay;
        }
        const int64_t budget =
            std::max<int64_t>(rtt_us, 0) / 2 + 2 * std::max<int64_t>(jitter_us, 0) + kDelayMarginUs;
        const int64_t ticks = (budget + tick_us - 1) / tick_us;
        return static_cast<uint8_t>(std::clamp<int64_t>(ticks, kMinDelay, kMaxDelay));
    }

    InputSync() = default;
    InputSync(const InputSync &) = delete;
    InputSync &operator=(const InputSync &) = delete;

    /// Begin a match on `port`, which must outlive it (or stop()). False,
    /// and still Idle, if `start` isn't valid() or this unit's slot isn't one
    /// of its players. Restarting drops whatever the last match had.
    bool start(SyncPort &port, const SyncStart &start);

    /// Once a frame: queue `local` (input_size bytes; shorter is zero-filled)
    /// for the ticks that come due, run what can run, and send this frame's
    /// packets. Never blocks.
    const SyncFrame &advance(std::span<const uint8_t> local);
    /// The same with a 2-byte pad record (little-endian)
    const SyncFrame &advance(uint16_t pad) {
        const uint8_t b[2] = {static_cast<uint8_t>(pad), static_cast<uint8_t>(pad >> 8)};
        return advance(std::span<const uint8_t>(b, 2));
    }

    /// A packet from `from_slot` (a kMessageType message's body). Untrusted:
    /// checked, and counted in stats() whatever happens to it.
    void receive(uint8_t from_slot, std::span<const uint8_t> packet);

    /// The game's state hash after running `tick` (one the frame marked
    /// hash_due). Compared with the other players' as theirs arrive.
    void report_hash(uint32_t tick, uint32_t hash);

    /// End the match here and tell the others (they get PeerQuit). Safe in
    /// any state; Idle stays Idle.
    void stop();

    SyncStatus status() const { return frame_.status; }
    bool started() const { return state_ != State::Idle; }
    bool running() const { return state_ == State::Running; }
    /// The next tick to run (= ticks run so far)
    uint32_t next_tick() const { return next_tick_; }
    /// The highest tick due now on the schedule, or -1 before tick 0
    int64_t due_tick() const;
    const SyncStart &params() const { return start_; }
    uint8_t local_slot() const { return local_; }
    /// The tick a player said it would run next, in its latest packet
    uint32_t peer_tick(uint8_t slot) const {
        return slot < kSyncMaxPlayers ? slots_[slot].peer_tick : 0;
    }
    int64_t shift_us() const { return shift_us_; }
    const SyncFrame &frame() const { return frame_; }
    SyncStats stats() const { return stats_; }

  private:
    enum class State : uint8_t { Idle, Running, Ended, Desynced };
    static constexpr uint32_t kNoTick = 0xFFFFFFFFu;
    static constexpr uint8_t kHashHistory = 4;

    struct Slot {
        std::array<std::array<uint8_t, kSyncMaxInputSize>, kRing> input{};
        std::array<uint32_t, kRing> tick_of{}; ///< tick held at each index, or kNoTick
        uint32_t acked = 0;                    ///< first tick of our input it lacks
        uint32_t peer_tick = 0;                ///< its next tick, as it last said
        int64_t waiting_since = -1;            ///< waiting on its input since, or -1
        bool quit = false;
        bool have_hash = false; ///< its latest hash not yet compared
        uint32_t hash_tick = 0;
        uint32_t hash = 0;
        uint32_t compared_tick = kNoTick; ///< its hash last compared
        // What was last sent to it, so a frame with nothing new sends nothing
        uint32_t sent_ack = kNoTick;
        uint32_t sent_local = 0; ///< next_local_ then
        uint32_t sent_hash_tick = kNoTick;
        int64_t sent_us = 0;

        bool has(uint32_t tick) const { return tick_of[tick % kRing] == tick; }
    };

    bool playing(uint8_t slot) const { return (start_.players >> slot) & 1; }
    int64_t due_us(uint32_t tick) const {
        return start_.start_us + shift_us_ + static_cast<int64_t>(tick) * start_.tick_us;
    }
    uint32_t ack_for(uint8_t slot) const;
    void finish(SyncEnd why, uint8_t slot);
    void desync(uint32_t tick, uint8_t slot);
    void compare(uint8_t slot, uint32_t tick, uint32_t hash);
    /// To every other player; with `force` false, only to those with
    /// something new for them or not sent to for a tick
    void send_packets(uint8_t flags, bool force);
    void report_over();

    SyncPort *port_ = nullptr;
    State state_ = State::Idle;
    SyncStart start_{};
    uint32_t match_ = 0;
    uint8_t local_ = kSyncNoSlot;
    uint32_t next_tick_ = 0;  ///< next tick to run
    uint32_t next_local_ = 0; ///< next tick to queue local input for
    int64_t shift_us_ = 0;
    std::array<Slot, kSyncMaxPlayers> slots_{};
    std::array<uint32_t, kHashHistory> own_hash_tick_{};
    std::array<uint32_t, kHashHistory> own_hash_{};
    uint8_t own_hashes_ = 0; ///< how many of own_hash_* are valid (newest at [0])
    SyncEnd end_ = SyncEnd::None;
    uint8_t end_slot_ = kSyncNoSlot;
    uint32_t desync_tick_ = 0;
    uint8_t desync_slot_ = kSyncNoSlot;
    SyncFrame frame_{};
    SyncStats stats_{};
    std::array<uint8_t, kMaxPacket> tx_{};
};

// ---- Implementation ---------------------------------------------------------

namespace sync_detail {

inline void put16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}
inline void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}
inline uint32_t get32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

} // namespace sync_detail

inline const char *to_string(SyncStatus s) {
    switch (s) {
    case SyncStatus::Idle:
        return "Idle";
    case SyncStatus::Starting:
        return "Starting";
    case SyncStatus::Running:
        return "Running";
    case SyncStatus::Waiting:
        return "Waiting";
    case SyncStatus::Lagging:
        return "Lagging";
    case SyncStatus::Ended:
        return "Ended";
    case SyncStatus::Desynced:
        return "Desynced";
    }
    return "?";
}

inline const char *to_string(SyncEnd e) {
    switch (e) {
    case SyncEnd::None:
        return "None";
    case SyncEnd::PeerLeft:
        return "PeerLeft";
    case SyncEnd::PeerQuit:
        return "PeerQuit";
    case SyncEnd::SessionEnded:
        return "SessionEnded";
    case SyncEnd::Stopped:
        return "Stopped";
    }
    return "?";
}

inline bool SyncStart::valid() const {
    return tick_us >= 1000 && tick_us <= 1'000'000 && input_delay >= InputSync::kMinDelay &&
           input_delay <= InputSync::kMaxDelay && input_size >= 1 &&
           input_size <= kSyncMaxInputSize && (players & ~0x0Fu) == 0 && player_count() >= 2;
}

inline int SyncStart::player_count() const {
    int n = 0;
    for (int s = 0; s < kSyncMaxPlayers; ++s) {
        n += (players >> s) & 1;
    }
    return n;
}

inline uint8_t SyncStart::timekeeper() const {
    for (uint8_t s = 0; s < kSyncMaxPlayers; ++s) {
        if ((players >> s) & 1) {
            return s;
        }
    }
    return kSyncNoSlot;
}

inline size_t SyncStart::encode(std::span<uint8_t> out) const {
    using namespace sync_detail;
    if (out.size() < kEncodedSize) {
        return 0;
    }
    uint8_t *p = out.data();
    p[0] = kVersion;
    put32(p + 1, seed);
    put64(p + 5, static_cast<uint64_t>(start_us));
    put32(p + 13, tick_us);
    p[17] = input_delay;
    p[18] = input_size;
    p[19] = players;
    return kEncodedSize;
}

inline bool SyncStart::decode(std::span<const uint8_t> in, SyncStart &out) {
    using namespace sync_detail;
    if (in.size() < kEncodedSize || in[0] != kVersion) {
        return false;
    }
    SyncStart s;
    const uint8_t *p = in.data();
    s.seed = get32(p + 1);
    s.start_us = static_cast<int64_t>(get64(p + 5));
    s.tick_us = get32(p + 13);
    s.input_delay = p[17];
    s.input_size = p[18];
    s.players = p[19];
    if (!s.valid()) {
        return false;
    }
    out = s;
    return true;
}

inline uint32_t SyncStart::match_id() const {
    std::array<uint8_t, kEncodedSize> b{};
    encode(b);
    return Fnv1a{}.add(b.data(), b.size()).value;
}

inline int64_t InputSync::due_tick() const {
    if (state_ == State::Idle || !port_) {
        return -1;
    }
    const int64_t since = port_->now_us() - start_.start_us - shift_us_;
    return since < 0 ? -1 : since / start_.tick_us;
}

inline bool InputSync::start(SyncPort &port, const SyncStart &start) {
    if (!start.valid()) {
        return false;
    }
    const uint8_t local = port.local_slot();
    if (local >= kSyncMaxPlayers || !((start.players >> local) & 1)) {
        return false;
    }
    port_ = &port;
    start_ = start;
    match_ = start.match_id();
    local_ = local;
    state_ = State::Running;
    next_tick_ = 0;
    next_local_ = start.input_delay;
    shift_us_ = 0;
    own_hashes_ = 0;
    end_ = SyncEnd::None;
    end_slot_ = kSyncNoSlot;
    desync_tick_ = 0;
    desync_slot_ = kSyncNoSlot;
    stats_ = {};
    frame_ = {};
    frame_.status = SyncStatus::Starting;
    for (uint8_t s = 0; s < kSyncMaxPlayers; ++s) {
        Slot &slot = slots_[s];
        slot.tick_of.fill(kNoTick);
        slot.acked = 0;
        slot.peer_tick = 0;
        slot.waiting_since = -1;
        slot.quit = false;
        slot.have_hash = false;
        slot.compared_tick = kNoTick;
        slot.sent_ack = kNoTick;
        slot.sent_local = 0;
        slot.sent_hash_tick = kNoTick;
        slot.sent_us = 0;
        // Ticks before the input delay run with every input zero
        for (uint32_t t = 0; t < start.input_delay; ++t) {
            slot.input[t].fill(0);
            slot.tick_of[t] = t;
        }
    }
    return true;
}

inline uint32_t InputSync::ack_for(uint8_t slot) const {
    const Slot &s = slots_[slot];
    uint32_t t = next_tick_;
    while (t < next_tick_ + kRing && s.has(t)) {
        ++t;
    }
    return t;
}

inline void InputSync::finish(SyncEnd why, uint8_t slot) {
    if (state_ != State::Running) {
        return;
    }
    state_ = State::Ended;
    end_ = why;
    end_slot_ = slot;
    // A stop here is news to the others (best effort: if it's lost they see
    // this unit lagging, and gone when it leaves the session). An end caused
    // by another player isn't: they each see that player go themselves, and
    // a quit from here would hide who it was.
    if (why == SyncEnd::Stopped) {
        send_packets(kFlagQuit, true);
    }
}

inline void InputSync::desync(uint32_t tick, uint8_t slot) {
    if (state_ != State::Running) {
        return;
    }
    state_ = State::Desynced;
    desync_tick_ = tick;
    desync_slot_ = slot;
}

inline void InputSync::compare(uint8_t slot, uint32_t tick, uint32_t hash) {
    if (slots_[slot].compared_tick == tick) {
        return; // it sends its latest hash in every packet
    }
    for (uint8_t i = 0; i < own_hashes_; ++i) {
        if (own_hash_tick_[i] == tick) {
            ++stats_.hashes_compared;
            if (own_hash_[i] != hash) {
                desync(tick, slot);
            }
            slots_[slot].have_hash = false;
            slots_[slot].compared_tick = tick;
            return;
        }
    }
    // Not hashed here yet: keep it until report_hash() for that tick
    Slot &s = slots_[slot];
    s.have_hash = true;
    s.hash_tick = tick;
    s.hash = hash;
}

inline void InputSync::report_hash(uint32_t tick, uint32_t hash) {
    if (state_ == State::Idle) {
        return;
    }
    if (own_hashes_ > 0 && own_hash_tick_[0] == tick) {
        own_hash_[0] = hash;
    } else {
        for (int i = kHashHistory - 1; i > 0; --i) {
            own_hash_tick_[i] = own_hash_tick_[i - 1];
            own_hash_[i] = own_hash_[i - 1];
        }
        own_hash_tick_[0] = tick;
        own_hash_[0] = hash;
        own_hashes_ = static_cast<uint8_t>(std::min<int>(own_hashes_ + 1, kHashHistory));
    }
    for (uint8_t s = 0; s < kSyncMaxPlayers; ++s) {
        if (s != local_ && slots_[s].have_hash && slots_[s].hash_tick == tick) {
            compare(s, slots_[s].hash_tick, slots_[s].hash);
        }
    }
}

inline void InputSync::receive(uint8_t from, std::span<const uint8_t> p) {
    using namespace sync_detail;
    if (state_ == State::Idle || state_ == State::Ended || from >= kSyncMaxPlayers ||
        from == local_ || !playing(from)) {
        ++stats_.ignored;
        return;
    }
    if (p.size() < 2) {
        ++stats_.malformed;
        return;
    }
    if (p[0] != kPacketVersion) {
        ++stats_.wrong_version;
        return;
    }
    if (p.size() < kHeaderSize) {
        ++stats_.malformed;
        return;
    }
    const uint8_t flags = p[1];
    if (get32(&p[2]) != match_) {
        ++stats_.ignored;
        return;
    }
    const uint32_t peer_tick = get32(&p[6]);
    const uint32_t ack = get32(&p[10]);
    const int64_t shift = static_cast<int64_t>(get64(&p[14]));
    const uint32_t first = get32(&p[22]);
    const uint8_t count = p[26];
    const size_t inputs = static_cast<size_t>(count) * start_.input_size;
    const bool hash = flags & kFlagHash;
    if ((flags & ~(kFlagHash | kFlagQuit)) != 0 || count > kMaxInputsPerPacket ||
        p.size() != kHeaderSize + inputs + (hash ? 8 : 0) || shift < 0 ||
        (count > 0 && first > kNoTick - count)) {
        ++stats_.malformed;
        return;
    }
    ++stats_.packets_received;
    Slot &s = slots_[from];
    s.peer_tick = std::max(s.peer_tick, peer_tick);
    // An ack can't be for input not queued yet; ignore what would be
    s.acked = std::max(s.acked, std::min(ack, next_local_));
    if (from == start_.timekeeper() && shift > shift_us_) {
        shift_us_ = shift; // follow the timekeeper's schedule
    }
    if (flags & kFlagQuit) {
        s.quit = true;
    }
    const uint8_t *in = &p[kHeaderSize];
    for (uint32_t i = 0; i < count; ++i, in += start_.input_size) {
        const uint32_t t = first + i;
        if (t < next_tick_ || t >= next_tick_ + kRing || s.has(t)) {
            ++stats_.inputs_redundant;
            continue;
        }
        auto &rec = s.input[t % kRing];
        rec.fill(0);
        std::memcpy(rec.data(), in, start_.input_size);
        s.tick_of[t % kRing] = t;
        ++stats_.inputs_received;
    }
    if (hash && state_ == State::Running) {
        compare(from, get32(in), get32(in + 4));
    }
}

inline void InputSync::send_packets(uint8_t extra_flags, bool force) {
    using namespace sync_detail;
    const Slot &mine = slots_[local_];
    const int64_t now = port_->now_us();
    for (uint8_t to = 0; to < kSyncMaxPlayers; ++to) {
        if (to == local_ || !playing(to)) {
            continue;
        }
        Slot &peer = slots_[to];
        const uint32_t ack = ack_for(to);
        const uint32_t hash_tick = own_hashes_ > 0 ? own_hash_tick_[0] : kNoTick;
        if (!force && ack == peer.sent_ack && next_local_ == peer.sent_local &&
            hash_tick == peer.sent_hash_tick && now - peer.sent_us < start_.tick_us) {
            continue; // nothing new (waiting, polled again within a tick)
        }
        peer.sent_ack = ack;
        peer.sent_local = next_local_;
        peer.sent_hash_tick = hash_tick;
        peer.sent_us = now;
        // Everything it hasn't acknowledged, oldest first; never the
        // all-zero ticks before the delay, nor more than the ring still has
        const uint32_t oldest = next_local_ > kRing ? next_local_ - kRing : 0;
        const uint32_t from = std::max({peer.acked, oldest, uint32_t{start_.input_delay}});
        const uint32_t count =
            from < next_local_ ? std::min(next_local_ - from, kMaxInputsPerPacket) : 0;
        uint8_t flags = extra_flags;
        if (own_hashes_ > 0) {
            flags |= kFlagHash;
        }
        uint8_t *p = tx_.data();
        p[0] = kPacketVersion;
        p[1] = flags;
        put32(p + 2, match_);
        put32(p + 6, next_tick_);
        put32(p + 10, ack);
        put64(p + 14, static_cast<uint64_t>(shift_us_));
        put32(p + 22, from);
        p[26] = static_cast<uint8_t>(count);
        size_t n = kHeaderSize;
        for (uint32_t i = 0; i < count; ++i) {
            std::memcpy(p + n, mine.input[(from + i) % kRing].data(), start_.input_size);
            n += start_.input_size;
        }
        if (flags & kFlagHash) {
            put32(p + n, own_hash_tick_[0]);
            put32(p + n + 4, own_hash_[0]);
            n += 8;
        }
        if (port_->send(to, std::span<const uint8_t>(p, n))) {
            ++stats_.packets_sent;
            stats_.inputs_sent += count;
            stats_.max_inputs_in_packet = std::max(stats_.max_inputs_in_packet, count);
        } else {
            ++stats_.send_failed;
        }
    }
}

inline void InputSync::report_over() {
    frame_.status = state_ == State::Desynced ? SyncStatus::Desynced : SyncStatus::Ended;
    frame_.end = end_;
    frame_.end_slot = end_slot_;
    frame_.desync_tick = desync_tick_;
    frame_.desync_slot = desync_slot_;
    frame_.interval_us = start_.tick_us;
}

inline const SyncFrame &InputSync::advance(std::span<const uint8_t> local) {
    frame_.ticks = 0;
    frame_.waiting_for = 0;
    frame_.lagging = 0;
    if (state_ == State::Idle) {
        frame_.status = SyncStatus::Idle;
        frame_.interval_us = kDefaultTickUs;
        return frame_;
    }
    if (state_ == State::Desynced) {
        // Keep sending, so the others get this unit's hash and see it too
        send_packets(0, false);
    }
    if (state_ != State::Running) {
        report_over();
        return frame_;
    }
    ++stats_.frames;

    // Who's still here
    if (port_->local_slot() != local_) {
        finish(SyncEnd::SessionEnded, local_);
    }
    for (uint8_t s = 0; s < kSyncMaxPlayers && state_ == State::Running; ++s) {
        if (s == local_ || !playing(s)) {
            continue;
        }
        if (!port_->present(s)) {
            finish(SyncEnd::PeerLeft, s);
        } else if (slots_[s].quit) {
            finish(SyncEnd::PeerQuit, s);
        }
    }
    if (state_ != State::Running) {
        report_over();
        return frame_;
    }

    const int64_t now = port_->now_us();
    const int64_t tick_us = start_.tick_us;
    if (now < due_us(0)) {
        frame_.status = SyncStatus::Starting;
        frame_.interval_us = std::clamp<int64_t>(due_us(0) - now, kMinIntervalUs, tick_us);
        return frame_;
    }
    int64_t due = (now - start_.start_us - shift_us_) / tick_us;
    // Far behind (a long stall): the timekeeper moves the schedule back and
    // the others follow its shift, instead of a long burst of catching up
    if (local_ == start_.timekeeper() && due > static_cast<int64_t>(next_tick_) + kMaxBehindTicks) {
        const int64_t back = due - next_tick_ - kMaxBehindTicks;
        shift_us_ += back * tick_us;
        due -= back;
        ++stats_.shifts;
        stats_.shift_us += back * tick_us;
    }

    std::array<uint8_t, kSyncMaxInputSize> pad{};
    std::memcpy(pad.data(), local.data(), std::min<size_t>(local.size(), start_.input_size));
    uint8_t missing = 0;
    while (frame_.ticks < SyncFrame::kMaxTicks && static_cast<int64_t>(next_tick_) <= due) {
        // Tick next_tick_ is due: this frame's input goes to next_tick_ + D
        Slot &mine = slots_[local_];
        while (next_local_ <= next_tick_ + start_.input_delay) {
            mine.input[next_local_ % kRing] = pad;
            mine.tick_of[next_local_ % kRing] = next_local_;
            ++next_local_;
        }
        for (uint8_t s = 0; s < kSyncMaxPlayers; ++s) {
            if (playing(s) && !slots_[s].has(next_tick_)) {
                missing |= static_cast<uint8_t>(1u << s);
            }
        }
        if (missing) {
            break;
        }
        SyncTick &t = frame_.tick[frame_.ticks++];
        t.number = next_tick_;
        t.hash_due = (next_tick_ + 1) % kHashInterval == 0;
        t.input_size = start_.input_size;
        for (uint8_t s = 0; s < kSyncMaxPlayers; ++s) {
            if (playing(s)) {
                t.inputs[s] = slots_[s].input[next_tick_ % kRing];
            } else {
                t.inputs[s].fill(0);
            }
        }
        ++next_tick_;
    }
    stats_.ticks += frame_.ticks;
    if (frame_.ticks > 1) {
        ++stats_.catchup_frames;
    }

    for (uint8_t s = 0; s < kSyncMaxPlayers; ++s) {
        Slot &slot = slots_[s];
        if (missing & (1u << s)) {
            if (slot.waiting_since < 0) {
                slot.waiting_since = now;
            }
            if (now - slot.waiting_since >= kLagUs) {
                frame_.lagging |= static_cast<uint8_t>(1u << s);
            }
        } else {
            slot.waiting_since = -1;
        }
    }
    frame_.waiting_for = missing;
    if (missing) {
        ++stats_.stall_frames;
        frame_.status = frame_.lagging ? SyncStatus::Lagging : SyncStatus::Waiting;
        frame_.interval_us = kRetryUs;
    } else {
        frame_.status = SyncStatus::Running;
        frame_.interval_us = std::clamp<int64_t>(due_us(next_tick_) - now, kMinIntervalUs, tick_us);
    }
    send_packets(0, false);
    return frame_;
}

inline void InputSync::stop() {
    if (state_ == State::Running) {
        finish(SyncEnd::Stopped, local_);
    } else if (state_ == State::Desynced) {
        send_packets(kFlagQuit, true);
        state_ = State::Ended;
        end_ = SyncEnd::Stopped;
        end_slot_ = local_;
    }
    if (state_ != State::Idle) {
        report_over();
    }
}

} // namespace game
