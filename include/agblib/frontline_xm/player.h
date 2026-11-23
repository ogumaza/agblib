// SPDX-License-Identifier: MIT

// A model of Frontline Studios' XM-like player and its mixer (driver.h), a frame at a time. It keeps the player's
// state and the four channels' records as the player's code does (Chicken Shoot: the mix at 0x080E5974, the tick at
// 0x080E5A44, the row at 0x080E5A80, the effects from 0x080E5CB0 and the tick routines from 0x080E6234), and mixes
// each frame's samples as the mixer that the player copies to IWRAM does (0x03000268). It writes the bytes that the
// game gives FIFO A.
//
// The model follows the player's quirks: a delayed note sets the sounding period but not the channel's period, and the
// next row without a note therefore goes back to the old pitch; a slide to a note in the volume column stops the
// channel's effect, not itself, when it arrives; the tremolo changes the volume for good; effect 8 runs the cell's
// volume byte again. The tick routines of effects 11 and 12 run the whole tick again. The model doesn't follow those
// effects: Unmodelled() counts them.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"

namespace agblib::frontline_xm
{

// What a channel runs at each tick that isn't a row: the routines of the effect and of the volume column.
enum class Routine : uint8_t
{
    kNone,
    kSlideUp,
    kSlideDown,
    kSlideToNote,
    kVolumeSlide,
    kVibrato,
    kArpeggio,
    kRepeatTick, // effects 11 and 12: not followed
    kCut,
    kRetrigger,
    kTremolo,
    kJump,
    kDelay,
    kDelayNothing, // a delay under note handlers 2 and 3: they play no delayed note
};

// A channel's record (60 bytes in the player), with its offsets.
struct Channel
{
    uint32_t instrument = 0;         // 0x00: the instrument's record
    uint32_t points = 0;             // 0x04
    uint32_t end = 0;                // 0x08: in 1/8192 points
    uint32_t position = 0;           // 0x0C
    uint32_t step = 0;               // 0x10
    uint16_t period = 0;             // 0x14: the note's period, moved by the slides
    uint16_t on = 0;                 // 0x16
    Routine effect = Routine::kNone; // 0x1C
    Routine column = Routine::kNone; // 0x20
    uint32_t cell = 0;               // 0x24: the row's cell, read again by a delayed note
    uint16_t sounding = 0;           // 0x28: the period that plays
    uint16_t target = 0;             // 0x2A: a slide's target
    uint8_t counter = 0;             // 0x2C: an arpeggio's, a cut's, a delay's or a retrigger's ticks
    uint8_t retrigger = 0;           // 0x2D
    uint8_t slide_up = 0;            // 0x2E
    uint8_t slide_down = 0;          // 0x2F
    uint8_t slide_speed = 0;         // 0x30
    uint8_t arpeggio = 0;            // 0x31
    int8_t volume_slide = 0;         // 0x32
    uint8_t volume = 0;              // 0x33
    uint8_t vibrato_place = 0;       // 0x34
    uint8_t vibrato_depth = 0;       // 0x35
    uint8_t vibrato_speed = 0;       // 0x36
    uint8_t tremolo_place = 0;       // 0x37
    uint8_t tremolo_depth = 0;       // 0x38
    uint8_t tremolo_speed = 0;       // 0x39
    uint8_t loop_type = 0;           // 0x3A

    // For conversions, not in the player's record: the starts of the channel's sample, at a note or an offset, and the
    // instrument's number in its module.
    uint32_t starts = 0;
    uint8_t number = 0;
};

class Player
{
public:
    // Keeps references to `rom` and `info`. They have to outlive the player.
    Player(const Rom& rom, const DriverInfo& info);

    // Starts song `song` of the driver's list: the load of its module, the play of its list, and its master volume.
    void Start(int song);

    // Mixes one frame, with the ticks that fall in it.
    void Frame();

    // The bytes of the last frame's mix.
    const std::vector<int8_t>& Output() const;

    // Called after each tick, before the mix of the samples after it.
    std::function<void()> on_tick;

    uint8_t Order() const;
    uint8_t Row() const;
    uint8_t TickCount() const;
    uint8_t Speed() const;
    uint8_t Bpm() const;
    uint8_t Master() const;
    uint16_t TickSamples() const;
    uint16_t SamplesToTick() const;
    uint32_t FrameSamples() const;
    const Channel& ChannelAt(int c) const;

    // The samples that the mixer has written since the song's start.
    uint64_t Elapsed() const;

    // Whether the last tick read a row, and that row's order and number in its pattern.
    bool RowRead() const;
    uint8_t RowOrder() const;
    uint8_t RowNumber() const;

    // Returns how often the player reached a path that the model doesn't follow.
    int Unmodelled() const;

private:
    void Load(const Module& module);
    void Play(uint32_t list, uint8_t first, uint8_t last);
    void LoadPattern(uint8_t order);
    void Tick();
    void ReadRow();
    void Note(Channel& c, uint8_t note, uint8_t instrument, uint8_t effect, int32_t& column);
    void StartNote(Channel& c, uint8_t note, uint8_t instrument);
    int32_t Column(Channel& c, int32_t value);
    void RunEffect(Channel& c, uint8_t effect, uint8_t parameter, int32_t column);
    void RunRoutine(Channel& c, Routine routine);
    void DelayedNote(Channel& c);
    void SetSteps();
    void MixSegment(uint32_t from, uint32_t count, uint8_t master);
    int8_t Point(uint32_t address);
    uint16_t SamplesPerTick(uint8_t bpm);
    uint16_t Period(uint8_t note, uint32_t instrument) const;

    const Rom& rom_;
    const DriverInfo& info_;

    uint16_t frame_samples_ = 0; // 0x00
    uint16_t left_ = 0;          // 0x02: the samples to the next tick
    uint16_t tick_samples_ = 0;  // 0x04
    uint8_t bpm_ = 0;            // 0x06
    uint8_t master_ = 0;         // 0x0A
    uint8_t pattern_delay_ = 0;  // 0x0B: kept, never read
    uint8_t note_handler_ = 0;   // 0x0C
    uint8_t jump_order_ = 0;     // 0x0D
    uint8_t restart_ = 0;        // 0x0E
    uint8_t break_row_ = 0;      // 0x0F
    uint8_t tick_ = 0;           // 0x34
    uint8_t speed_ = 0;          // 0x35
    uint8_t row_ = 0;            // 0x36
    uint8_t rows_ = 0;           // 0x38
    uint8_t order_ = 0;          // 0x39
    uint8_t last_ = 0;           // 0x3A
    uint8_t channel_count_ = 0;  // 0x3B
    uint32_t patterns_ = 0;      // 0x3C
    uint32_t instruments_ = 0;   // 0x40
    uint32_t data_ = 0;          // 0x44
    uint32_t list_ = 0;          // 0x48
    uint32_t vibrato_wave_ = 0;  // 0x4C
    uint32_t tremolo_wave_ = 0;  // 0x50

    Channel channels_[kChannels];
    std::vector<int32_t> sums_;
    std::vector<int8_t> output_;
    int unmodelled_ = 0;
    uint64_t elapsed_ = 0;
    bool row_read_ = false;
    uint8_t row_order_ = 0;
    uint8_t row_number_ = 0;
};

} // namespace agblib::frontline_xm
