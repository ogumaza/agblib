// SPDX-License-Identifier: MIT

// A model of Engine Software's GBA sound driver (driver.h) and its mono mixer, a frame at a time, for the builds that
// DetectDriver() accepts. It keeps the driver's state and the channels' records as the driver's code does, and mixes
// each frame as the mixer that the driver copies to IWRAM does. It writes the bytes that the game gives FIFO A.
//
// The model follows The Santa Clause 3's build (B33E): the frame routine at 0x08000C14, the row at 0x08001970, the
// row's routines from 0x08001E54, the effects of the row from 0x0800203C and of the other ticks from 0x08001B1C, the
// envelopes from 0x08001D5C with the envelope's step at 0x08000D90, the volume and pitch at 0x080023A4, the chain of
// channels at 0x08000628 and the mixer at 0x08001844. A song starts as the game's sound manager starts it: the music
// volume at 0, then a ramp toward the manager's target, a step each frame before the frame routine. A jingle starts at
// the manager's music volume.
//
// The model follows the driver's faults: the vibrato replaces the slide rather than adding to it, and its last value
// stays; the tremolo changes the volume for good, with the vibrato's speed, depth and place; an ended sample that
// plays once keeps adding its last point to the mix where the chain skips a channel before it, up to the next tick. It
// doesn't follow a sample offset past the end of a sample that plays once (the driver's loop never ends), a pitch
// outside the pitch table, a sample outside the cartridge, a sum outside the clip table, a jingle that resumes a song,
// an instrument number past the bank's count or more than 8 channels. Unmodelled() counts them.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"

namespace agblib::engine_software
{

// The bits of a channel's flags.
constexpr uint16_t kChannelOff = 0x01;
constexpr uint16_t kChannelPaused = 0x02;
constexpr uint16_t kChannelReleased = 0x04;
constexpr uint16_t kChannelPitch = 0x08;  // the step has to be worked out again
constexpr uint16_t kChannelVolume = 0x10; // the level has to be worked out again

// An envelope's state in a channel's record.
struct EnvelopeState
{
    uint16_t tick = 0;
    uint16_t value = 0;
    uint16_t from = 0; // the point before the tick
    uint16_t to = 0;   // the point after it
};

// A channel's record (0x4C bytes in the driver), with its offsets.
struct Channel
{
    uint32_t handler = 0;          // 0x00: the mixer's handler, as its offset in the mixer's image
    uint16_t flags = 0;            // 0x04
    uint8_t delay = 0;             // 0x06: a delayed note's ticks
    uint8_t retrigger = 0;         // 0x07: the retrigger's ticks
    uint32_t instrument = 0;       // 0x08: the instrument's record
    uint8_t note = 0;              // 0x0C: the note that plays
    uint8_t base = 0;              // 0x0D: the row's note
    int8_t relative = 0;           // 0x0E: the instrument's relative note
    int8_t volume = 0;             // 0x0F: 0 to 64
    int8_t pan = 0;                // 0x10: -64 to 64
    uint8_t target = 0;            // 0x11: a slide's note
    int16_t fine_tune = 0;         // 0x12
    int16_t slide = 0;             // 0x14: in 128ths of a semitone
    uint16_t slide_speed = 0;      // 0x16
    int8_t volume_slide = 0;       // 0x18
    uint8_t vibrato_speed = 0;     // 0x19
    uint8_t vibrato_depth = 0;     // 0x1A
    uint8_t vibrato_place = 0;     // 0x1B
    uint32_t fade = 0;             // 0x1C: the fade-out's level, 0xFFFF at a note
    EnvelopeState volume_envelope; // 0x20
    EnvelopeState pan_envelope;    // 0x28
    uint32_t loop_bytes = 0;       // 0x30
    uint32_t right = 0;            // 0x34: the right side's level in stereo
    uint32_t level = 0;            // 0x38: the mixer's volume
    uint32_t end = 0;              // 0x3C
    uint32_t fraction = 0;         // 0x40
    uint32_t step = 0;             // 0x44: 16.16 points a sample
    uint32_t place = 0;            // 0x48: the address of the next point

    // For conversions, not in the driver's record: the count of the starts of a sample, and the first point and
    // instrument of the sample that the channel started last.
    uint32_t starts = 0;
    uint32_t sample = 0;
    uint32_t sample_instrument = 0;
};

// A channel's fields of a row.
struct Cell
{
    uint8_t note = 0;
    uint8_t instrument = 0;
    uint16_t volume = 0;
    uint16_t effect = 0;
    uint16_t operand = 0;
};

class Player
{
public:
    // Keeps references to `rom` and `info`. They have to outlive the player. Makes the game's set-up.
    Player(const Rom& rom, const DriverInfo& info);

    // Starts song `song` of `info.songs` as the game's sound manager does.
    void Start(int song);

    // Runs one frame: a step of the manager's ramp, the frame routine, then the vertical blank's swap.
    void Frame();

    // The bytes of the last frame's mix.
    const std::vector<int8_t>& Output() const;

    // Called after each tick, before the mix of the samples after it.
    std::function<void()> on_tick;

    // Whether the song plays: false from the row at which a jingle ends.
    bool Playing() const;

    uint8_t Order() const;

    // The song's passes through its orders: each return to its restart order counts one.
    uint8_t Passes() const;

    uint16_t Row() const;
    uint8_t TickCount() const;
    uint8_t Speed() const;
    uint8_t Tempo() const;
    uint8_t PatternDelay() const;
    uint16_t MusicVolume() const;
    uint16_t TickSamples() const;
    int MusicChannels() const;
    const Channel& ChannelAt(int c) const;

    // The span of the clip table's slope, in table entries: a level of a quarter of it plays a sample's points at full
    // scale.
    uint32_t ClipSpan() const;

    // The samples that the mixer has written since the song's start.
    uint64_t Elapsed() const;

    // Whether the last tick read a row, and that row's order and number in its pattern.
    bool RowRead() const;
    uint8_t RowOrder() const;
    uint16_t RowNumber() const;

    // Returns how often the driver reached a path that the model doesn't follow.
    int Unmodelled() const;

private:
    void SetUp();
    void SetMusicVolume(uint16_t volume);
    void SetMusicChannels(int count);
    void BuildClipTable();
    void PlaySong(int song, uint16_t flags);
    void Stop();
    void RunFrame();
    void Tick();
    void NextOrder(uint16_t row);
    void ReadRow();
    void StartInstrument(Channel& c, const Cell& cell);
    void StartNote(Channel& c, const Cell& cell);
    void VolumeColumn(Channel& c, const Cell& cell);
    void RowEffect(Channel& c, const Cell& cell);
    void TickEffect(Channel& c, const Cell& cell);
    void Envelopes();
    void StepEnvelope(uint16_t flags, uint32_t envelope, EnvelopeState& state);
    void Update(Channel& c);
    void Vibrato(Channel& c);
    void Tremolo(Channel& c);
    void Release(Channel& c);
    void ResetEnvelopes(Channel& c);
    void MarkVolumes();
    void BuildChain();
    void Mix(uint32_t count);
    uint32_t Instrument(uint8_t number);
    int8_t Point(uint32_t address);
    int32_t Divide(int32_t numerator, int32_t denominator);

    const Rom& rom_;
    const DriverInfo& info_;

    // The mixer's state.
    bool output_on_ = false;
    int music_ = 0;
    int effects_ = 0;
    std::vector<int8_t> clip_;
    uint32_t clip_span_ = 0;
    uint32_t chain_start_ = 0;
    std::array<std::vector<int8_t>, 2> buffers_;
    int buffer_ = 0;
    uint32_t written_ = 0; // the bytes of the frame's buffer that the mixer has written

    // The sequencer's state.
    uint16_t flags_ = 0;             // 0x00
    uint8_t song_channels_ = 0;      // 0x02
    uint8_t tick_ = 0;               // 0x03
    uint8_t speed_ = 0;              // 0x04
    uint8_t pattern_delay_ = 0;      // 0x05
    uint8_t row_bytes_ = 0;          // 0x06
    uint8_t order_ = 0;              // 0x07
    uint8_t last_ = 0;               // 0x08
    uint8_t restart_ = 0;            // 0x09
    uint8_t passes_ = 0;             // 0x0A
    uint8_t tempo_ = 0;              // 0x0B
    uint16_t row_ = 0;               // 0x0C
    uint16_t tick_samples_ = 0;      // 0x0E
    uint16_t left_ = 0;              // 0x10: the samples to the next tick
    uint16_t music_volume_ = 0;      // 0x16
    uint32_t order_list_ = 0;        // 0x18
    std::vector<uint32_t> patterns_; // 0x24
    uint32_t pattern_ = 0;           // 0x28
    std::array<Cell, 8> cells_;      // 0x30

    // The sound manager's music volume, its step and its target.
    int32_t manager_volume_ = 0;
    int32_t manager_step_ = 0;
    int32_t manager_target_ = 0;

    std::array<Channel, kMaxRecords + 1> channels_;
    std::vector<int8_t> output_;
    int unmodelled_ = 0;
    uint64_t elapsed_ = 0;
    bool row_read_ = false;
    uint8_t row_order_ = 0;
    uint16_t row_number_ = 0;
};

} // namespace agblib::engine_software
