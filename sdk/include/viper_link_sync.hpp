#pragma once

// game::InputSync (lockstep) over viper_link.h, for ELF apps: the
// game::SyncPort built-in games get from LinkSyncPort, made of viper_link_*
// calls. Needs set(VIPER_LIBS game) before project_elf(). Header-only; the
// game library still depends on nothing.
//
//     game::InputSync sync_;
//     viper_link::SyncPort port_{sync_};
//
//     // when viper_link_lobby() first returns VIPER_LINK_LOBBY_PLAYING
//     viper_link_start_t s = {.size = sizeof s};
//     viper_link_start(&s);
//     port_.start(s);              // sync_.start() with the agreed parameters
//
//     // every frame while playing, after viper_link_poll()
//     port_.pump();                // the packets that arrived, to sync_
//     const game::SyncFrame &f = sync_.advance(my_input);
//     for (int i = 0; i < f.ticks; ++i) { step(f.tick[i]); ... report_hash ... }
//     return f.interval_ms();
//
//     // on VIPER_LINK_EVENT_BACK_TO_ROOM or _ENDED, or leaving the game
//     sync_.stop();
//
// The rules for the game are InputSync's (components/game/README.md): a
// fixed tick, integer state, a game::Rng seeded from the start's seed, and
// nothing read but the inputs.

#include <cstdint>
#include <span>

#include "game/input_sync.hpp"
#include "viper_link.h"

namespace viper_link {

/// The parameters InputSync::start() takes, from viper_link_start()'s
inline game::SyncStart sync_start(const viper_link_start_t &s) {
    game::SyncStart start;
    start.seed = s.seed;
    start.start_us = s.start_us;
    start.tick_us = s.tick_us;
    start.input_delay = s.input_delay;
    start.input_size = s.input_size;
    start.players = s.players;
    return start;
}

class SyncPort final : public game::SyncPort {
  public:
    explicit SyncPort(game::InputSync &sync)
        : sync_(sync) {
        status_.local_slot = VIPER_LINK_NO_SLOT;
    }

    SyncPort(const SyncPort &) = delete;
    SyncPort &operator=(const SyncPort &) = delete;

    /// Starts the InputSync on what every unit agreed; false if it refused
    /// (this unit isn't one of the players, or the parameters are bad)
    bool start(const viper_link_start_t &s) {
        // (packets left from an earlier match are told apart by its match id)
        refresh();
        return sync_.start(*this, sync_start(s));
    }

    /// Once a frame before InputSync::advance(): hands it the packets that
    /// arrived, and notes who's still in the room
    void pump() {
        refresh();
        uint8_t from = 0;
        int n;
        while ((n = viper_link_sync_receive(&from, buf_, sizeof buf_)) > 0) {
            if (static_cast<size_t>(n) <= sizeof buf_) {
                sync_.receive(from, std::span<const uint8_t>(buf_, static_cast<size_t>(n)));
            }
        }
    }

    // game::SyncPort
    int64_t now_us() const override { return viper_link_now_us(); }
    uint8_t local_slot() const override {
        return status_.local_slot == VIPER_LINK_NO_SLOT ? game::kSyncNoSlot : status_.local_slot;
    }
    bool present(uint8_t slot) const override {
        for (int i = 0; i < status_.player_count; ++i) {
            if (status_.players[i].slot == slot) {
                return true;
            }
        }
        return false;
    }
    bool send(uint8_t to_slot, std::span<const uint8_t> packet) override {
        return viper_link_sync_send(to_slot, packet.data(), packet.size()) == VIPER_LINK_OK;
    }

  private:
    void refresh() {
        status_ = {};
        status_.size = sizeof status_;
        if (viper_link_status(&status_) != VIPER_LINK_OK) {
            status_ = {};
            status_.local_slot = VIPER_LINK_NO_SLOT;
        }
    }

    game::InputSync &sync_;
    viper_link_status_t status_{};
    uint8_t buf_[VIPER_LINK_MAX_SYNC_PACKET] = {};
};

} // namespace viper_link
