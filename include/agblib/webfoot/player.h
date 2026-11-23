// SPDX-License-Identifier: MIT

// A model of Webfoot Technologies' GBA sound driver: the player of a song's rows and effects, and the mixer of its 15
// channels. The model keeps the driver's variables and steps them as the driver's code does, tick by tick and sample by
// sample. The addresses in the comments are those of Dragon Ball Z: The Legacy of Goku. That game has the 2002 build.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/webfoot/driver.h"

namespace agblib::webfoot
{

// The routine that a channel runs at each tick between rows: the driver keeps its address in the channel's record.
enum class TickRoutine : uint8_t
{
    kNone,
    kVolumeDown,          // 0x08009AD2
    kVolumeUp,            // 0x08009AFE
    kSlideDown,           // 0x08009BA6
    kSlideUp,             // 0x08009C34
    kPortaDown,           // 0x08009CC2
    kPortaUp,             // 0x08009D00
    kVibrato,             // 0x08009DB0
    kVibratoVolumeDown,   // 0x08009E7E
    kVibratoVolumeUp,     // 0x08009EAC
    kRetrigger,           // 0x0800A092
    kNoteDelay,           // 0x0800A0E4
    kPortaUpVolumeUp,     // 0x0800A182
    kPortaDownVolumeUp,   // 0x0800A1EE
    kPortaUpVolumeDown,   // 0x0800A28C
    kPortaDownVolumeDown, // 0x0800A2F8
};

// The bits of a channel's flags.
constexpr uint8_t kChannelTickMode = 0x03; // 1: a tick routine while the note sounds, 2: a tick routine always
constexpr uint8_t kChannelSounding = 0x04; // a note sounds
constexpr uint8_t kChannelTarget = 0x08;   // a slide to a note has its target

// A channel of the player: the driver's record, at the offsets of the 2002 build.
struct Channel
{
    uint32_t step = 0;                        // 0x00: the note's step, before the sample's rate
    uint32_t target = 0;                      // 0x04: the step that a slide to a note ends at
    int8_t volume = 0;                        // 0x08: 0 to 64
    int8_t pan = 0;                           // 0x09: -64 (left) to 64 (right)
    uint8_t flags = 0;                        // 0x0A
    uint8_t slide = 0;                        // 0x0B: what a tick of a volume slide adds or takes away
    uint8_t volume_memory = 0;                // 0x0C: the last parameter of D, K and L
    uint8_t pitch_memory = 0;                 // 0x0D: the last parameter of E, F and G
    uint8_t retrigger_memory = 0;             // 0x0E: the last parameter of Q
    uint8_t vibrato_speed = 0;                // 0x0F
    uint8_t vibrato_depth = 0;                // 0x10
    uint8_t vibrato_place = 0;                // 0x11: 0 to 63
    uint8_t counter = 0;                      // 0x12: the ticks to a retrigger or a delayed note
    TickRoutine routine = TickRoutine::kNone; // 0x14
    uint8_t mask = 0;                         // 0x18: the row's mask, or 0 if the row has no entry for the channel
    uint8_t last_mask = 0;                    // 0x19
    uint8_t note = 0;                         // 0x1A
    uint8_t instrument = 0;                   // 0x1B
    uint8_t volume_byte = 0;                  // 0x1C
    uint8_t effect = 0;                       // 0x1D
    uint8_t parameter = 0;                    // 0x1E
};

// The bits of a voice's flags. The voice routine (0x0800A748) applies the changes at the end of each tick.
constexpr uint8_t kVoiceOn = 0x01;
constexpr uint8_t kVoicePitch = 0x02;   // work the step out again
constexpr uint8_t kVoiceRestart = 0x04; // start the sample from its first point
constexpr uint8_t kVoiceSample = 0x08;  // load the sample's entry
constexpr uint8_t kVoiceLevels = 0x10;  // work the levels out again

// The bit of a voice's sample flags that the mixer sets while a ping-pong loop plays backwards.
constexpr uint8_t kVoiceBackwards = 0x04;

// A voice of the mixer: the driver's record of a music channel's sample, at the offsets of the 2002 build.
struct Voice
{
    int32_t position = 0;     // 0x00: in 16384ths of a point
    int32_t step = 0;         // 0x04: below 0 while a ping-pong loop plays backwards
    uint32_t points = 0;      // 0x08
    uint8_t flags = 0;        // 0x0C
    uint8_t sample_flags = 0; // 0x0D: the sample's flags, and kVoiceBackwards
    uint16_t left = 0;        // 0x0E: a byte in the 2002 build, a halfword in the later one (0x0C there)
    uint16_t right = 0;       // 0x0F: as `left` (0x0E in the later build)
    uint8_t volume = 0;       // 0x10
    int8_t pan = 0;           // 0x11
    uint16_t rate = 0;        // 0x12: the sample's rate at the middle note
    uint32_t speed = 0;       // 0x18: the step's size
    int32_t loop_start = 0;   // 0x1C: in 16384ths of a point
    int32_t end = 0;          // 0x20: the sample's length, in 16384ths of a point
    uint32_t sample = 0;      // 0x24: the address of the sample's entry
    uint32_t pitch = 0;       // 0x2C: the channel's step, with a vibrato

    // What the driver doesn't keep: the count of the times that a note or a retrigger starts the sample.
    uint32_t starts = 0;
};

// Plays a song as the game's driver does.
class Player
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this. `music_volume` is the game's setting of the music's
    // volume, from 0 to kFullMusicVolume.
    Player(const Rom& rom, const DriverInfo& info, int song, int music_volume = kStartMusicVolume);

    // Returns false if the song can't be read.
    bool Valid() const;

    const SongHeader& Header() const;
    const Mixer& MixerRules() const;

    // Sets a function that the player calls after each tick, with true if the tick read a row.
    void OnStep(std::function<void(bool)> callback);

    // Mixes `samples` samples. If `out` isn't null, appends each sample's left and right bytes as the FIFOs play them.
    void Mix(uint32_t samples, std::vector<int8_t>* out);

    // Returns the samples mixed from the song's start up to the last tick.
    uint64_t Elapsed() const;

    // The last row that the player read: its order, its number in its pattern, and its length in samples at the speed
    // and tempo that it starts with.
    int RowOrder() const;
    int RowNumber() const;
    uint32_t RowSamples() const;

    uint32_t Speed() const;
    uint32_t Tempo() const;

    // Returns what decides, besides the place in the song, how the song plays on: the speed and the tempo.
    std::vector<uint32_t> RepeatState() const;

    const std::array<Channel, kChannels>& Channels() const;
    const std::array<Voice, kChannels>& Voices() const;

    // Returns false once the player has met something that the driver can't play.
    bool Playing() const;

    const std::vector<std::string>& Warnings() const;

private:
    // The driver's routines.
    void Tick();
    void ReadRow();
    void RowEffect(int c);
    void Effect(int c, uint8_t effect);
    void Note(int c);
    void RunTickRoutine(int c);
    void VolumeDown(int c);
    void VolumeUp(int c);
    void SlideBy(int c, uint32_t factor, int shift);
    void PortaClamp(int c, TickRoutine then, bool volume_up);
    void VolumePart(int c, bool up);
    void Vibrato(int c);
    void VolumeSlideSetting(int c, TickRoutine up, TickRoutine down, TickRoutine level);
    void UpdateVoice(int c);
    void MixVoice(int c, uint32_t samples, std::vector<uint32_t>& sums);
    void AddPoints(const Voice& v, int32_t count, uint32_t place, std::vector<uint32_t>& sums);
    void SetPitch(int c, uint32_t step);
    void SetVolume(int c);
    void Warn(const std::string& text);

    uint32_t StepOf(uint8_t note) const;
    uint16_t SlideUp(uint32_t index) const;
    uint16_t SlideDown(uint32_t index) const;
    uint16_t FineUp(uint32_t index) const;
    uint16_t FineDown(uint32_t index) const;

    const Rom& rom_;
    Tables tables_;
    Build build_ = Build::k2003;
    Mixer mixer_;
    SongHeader header_;
    bool valid_ = false;
    bool playing_ = false;
    int music_volume_ = kStartMusicVolume;
    std::function<void(bool)> on_step_;
    std::vector<std::string> warnings_;

    // The driver's variables (0x03003CFC).
    uint8_t row_ = 0;
    uint8_t rows_ = 1;
    uint8_t tick_counter_ = 1;
    uint8_t speed_ = 0;
    uint8_t tempo_ = 0;
    int16_t order_ = -1;
    uint32_t tick_samples_ = 0;
    uint32_t samples_left_ = 0;
    uint32_t row_address_ = 0;
    std::array<Channel, kChannels> channels_ = {};
    std::array<Voice, kChannels> voices_ = {};

    // What the driver doesn't keep.
    uint64_t elapsed_ = 0;
    uint64_t mixed_ = 0;
    uint32_t mix_samples_ = 0; // the samples of a run of the mix routine: the model mixes its blocks
    bool row_read_ = false;
    int row_order_ = 0;
    int row_number_ = 0;
    uint32_t row_samples_ = 0;
};

} // namespace agblib::webfoot
