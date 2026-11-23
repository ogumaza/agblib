// SPDX-License-Identifier: MIT

// A model of Frontline Studios' MOD player and its mixer, stepped a frame at a time as the game's vertical blank
// steps them: the player plays a tick or a row, then the mixer writes a frame's samples for FIFO A. The model keeps
// the driver's records and follows its code. Its output is the bytes that the mixer writes.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/frontline_mod/driver.h"

namespace agblib::frontline_mod
{

// The routine that a channel runs on the ticks between rows, from the effect of its last cell.
enum class Tick
{
    kNone,
    kSlideDown,
    kSlideUp,
    kSlideToNote,
    kVolumeSlide,
    kVibrato,
    kVolumeVibrato,
};

// A channel's record of 64 bytes. The player and the mixer share it. The offsets are the record's.
struct Channel
{
    uint32_t on = 0;                // +0x00: 2 while the channel plays
    uint32_t step = 0;              // +0x04: 16.16 points a sample
    uint32_t fraction = 0;          // +0x08: the place within a point, in 65536ths
    uint32_t position = 0;          // +0x0C: the address of the next point
    uint32_t end = 0;               // +0x10: the sample's end
    uint32_t loop_start = 0;        // +0x14
    uint32_t loop_end = 0;          // +0x18: or 0 for a sample without a loop
    uint32_t level = 0;             // +0x1C: the volume times the music's volume over 256, set at each row
    Tick tick = Tick::kNone;        // +0x20
    uint32_t parameter = 0;         // +0x24: the effect's parameter
    uint32_t vibrato_phase = 0;     // +0x28: 0 to 63
    int32_t period = 0;             // +0x2C: the period index
    uint32_t vibrato_parameter = 0; // +0x30
    uint32_t target = 0;            // +0x34: the period index of the cell's note
    uint32_t memory = 0;            // +0x38: the last instrument; the sample offset keeps it too
    uint32_t volume = 0;            // +0x3C: 0 to 64

    // For the conversion: the instrument whose sample plays, a count of the notes and sample offsets that start a
    // sample, and a count of the mixer's restarts of the sample's loop since the last start.
    uint32_t instrument = 0;
    uint32_t starts = 0;
    uint32_t wraps = 0;
};

class Player
{
public:
    // A player of `info`'s driver in `rom`. Both have to outlive the player.
    Player(const Rom& rom, const DriverInfo& info);

    // Starts `module` at order `order` as the game's start does, with the fade step `step`. The channels keep what they
    // contain as on the console: a new player starts with every channel off.
    void Start(const Module& module, uint32_t order, int32_t step);

    // Runs the player, then the mixer.
    void Frame();

    // Runs the player: a tick, or a row at the speed. Returns true if it read a row.
    bool Play();

    // Runs the mixer: a frame's samples into Output().
    void Mix();

    const std::vector<int8_t>& Output() const;

    bool Playing() const;
    uint32_t Order() const;
    uint32_t Row() const;
    uint32_t Speed() const;
    int32_t MusicVolume() const;
    int32_t FadeStep() const;
    const Channel& ChannelAt(uint32_t channel) const;
    uint32_t MusicChannels() const;

    // The count of times the player or the mixer reached a path that the model doesn't follow: an effect whose handler
    // isn't one of the table's, an instrument of 0 with none before it, or a sum outside the clip table.
    uint64_t Unmodelled() const;

private:
    void ReadRow();
    uint32_t ReadNote(Channel& c, uint8_t flags, uint32_t at);
    void RunEffect(Channel& c, int effect, uint32_t parameter, bool note, bool& jumped);
    void RunTick(Channel& c);
    void SetStep(Channel& c, int32_t index);
    void Vibrato(Channel& c, uint32_t parameter);
    void MoveTo(uint32_t order);

    const Rom& rom_;
    const DriverInfo& info_;
    std::array<int8_t, 64> vibrato_{};
    std::array<int8_t, 2048> clip_{};

    // The player's state.
    uint32_t on_ = 0;
    int32_t tick_ = 0;
    int32_t speed_ = 0;
    uint32_t header_ = 0;
    uint32_t pattern_ = 0;
    uint32_t data_ = 0;
    uint32_t order_ = 0;
    uint32_t row_ = 0;

    // The mixer's context.
    int32_t music_volume_ = 0;
    int32_t fade_step_ = 0;
    std::vector<Channel> channels_;
    std::vector<uint32_t> sums_;
    std::vector<int8_t> output_;
    uint64_t unmodelled_ = 0;
};

} // namespace agblib::frontline_mod
