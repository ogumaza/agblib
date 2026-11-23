// SPDX-License-Identifier: MIT

// Definitions that the parts of the Quintet conversion share (convert.cpp, convert_simulation.h and convert_notes.h):
// the names they use, the constants of the MIDI file, and the simulation of a song and its notes. Only those files
// include this header. The definitions therefore keep internal linkage.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "agblib/agb/files.h"
#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/quintet/driver.h"
#include "agblib/quintet/sequencer.h"
#include "agblib/quintet/song.h"
#include "agblib/sequence_export/agb/beat_midi.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/psg.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/agb/song_banks.h"
#include "agblib/sequence_export/quintet/convert.h"
#include "agblib/sequence_export/quintet/soundfont.h"

namespace agblib::sequence_export::quintet
{

using agblib::agb::FrameSeconds;
using agblib::agb::kFrameCycles;
using agblib::agb::LoopLength;
using agblib::agb::LoopStart;
using agblib::agb::QuarterMicros;
using agblib::agb::TickSeconds;
using agblib::quintet::EffectIndex;
using agblib::quintet::Event;
using agblib::quintet::Fifo;
using agblib::quintet::FifoFrames;
using agblib::quintet::Hardware;
using agblib::quintet::kChannels;
using agblib::quintet::kFrequencyEntries;
using agblib::quintet::kNoise;
using agblib::quintet::kNr10;
using agblib::quintet::kNr30;
using agblib::quintet::kNr31;
using agblib::quintet::kNr41;
using agblib::quintet::kNr43;
using agblib::quintet::kNr50;
using agblib::quintet::kPcmA;
using agblib::quintet::kPsgControl;
using agblib::quintet::kPsgEnvelope;
using agblib::quintet::kSoundcntH;
using agblib::quintet::kWave;
using agblib::quintet::PsgRate;
using agblib::quintet::SampleCycles;
using agblib::quintet::Sequencer;
using agblib::quintet::SongAddress;
using agblib::quintet::TrackStretch;
using agblib::sequence_export::agb::AddressName;
using agblib::sequence_export::agb::BankProgram;
using agblib::sequence_export::agb::BeatTicks;
using agblib::sequence_export::agb::BendRangeFor;
using agblib::sequence_export::agb::ChannelWriter;
using agblib::sequence_export::agb::KeyHz;
using agblib::sequence_export::agb::kFifoLevel;
using agblib::sequence_export::agb::kPsgLevelPerVolume;
using agblib::sequence_export::agb::LevelsToControllers;
using agblib::sequence_export::agb::Looped;
using agblib::sequence_export::agb::MidiFile;
using agblib::sequence_export::agb::MidiTrack;
using agblib::sequence_export::agb::PsgFeed;
using agblib::sequence_export::agb::PsgModel;
using agblib::sequence_export::agb::PsgRegisters;
using agblib::sequence_export::agb::PsgScale;
using agblib::sequence_export::agb::PsgScaleOf;
using agblib::sequence_export::agb::PsgSemitones;
using agblib::sequence_export::agb::RootKeyZone;
using agblib::sequence_export::agb::SampleZone;
using agblib::sequence_export::agb::SettingCents;
using agblib::sequence_export::agb::SongNumber;
using agblib::sequence_export::agb::SongStem;
using agblib::sequence_export::agb::WaveKeyOffset;
using agblib::sequence_export::agb::WaveShare;

namespace
{

// The MIDI file's resolution: 37 ticks for each of the driver's. A frame at the driver's tempo T is then exactly T
// ticks long.
constexpr uint32_t kMidiTicksPerTick = 37;
constexpr uint32_t kTicksPerQuarter = 96 * kMidiTicksPerTick;

// The longest conversion, in frames: about an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The MIDI file's tempo when the model plays no frames.
constexpr int kDefaultTempo = 90;

// The slowest tempo that a MIDI file can express at this resolution: at 3 or less, a quarter note lasts more than the
// 2^24 microseconds a tempo event can give it.
constexpr int kMinMidiTempo = 4;

// Each channel's MIDI track name.
constexpr const char* kChannelNames[kChannels] = {"Square 1", "Square 2", "Wave", "Noise", "PCM A", "PCM B"};

// The key of the first drum in the noise channel's kit.
constexpr int kFirstDrumKey = 36;

// The MIDI key of a square channel's pitch table index 0 (C2). The wave channel plays a pattern of 32 steps an octave
// lower, and one of 64 steps two octaves lower.
constexpr int kSquareBaseKey = 36;

// A PCM note's pitch table index that plays a sample at its recorded rate, and the key that does in the SoundFont.
constexpr int kPcmUnityIndex = 24;
constexpr int kPcmRootKey = 60;

// The bits of SOUNDCNT_H for each FIFO: its volume (100% when set), and its right and left outputs.
constexpr uint16_t kFifoVolume[2] = {0x0004, 0x0008};
constexpr uint16_t kFifoRight[2] = {0x0100, 0x1000};
constexpr uint16_t kFifoLeft[2] = {0x0200, 0x2000};

// The state the model leaves after a frame, for the conversion.
struct FrameData
{
    Hardware hardware;
    std::array<Fifo, 2> fifos;
    std::array<uint16_t, kChannels> tempo = {};
};

// How the MIDI file times the events.
enum class Timing
{
    kBeat,   // on the song's ticks within the frames that the driver plays them in
    kFrames, // at the start of the frames that the driver plays them in
};

// The stretch of a song that the conversion covers, in MIDI ticks.
struct Plan
{
    bool loops = false;
    uint32_t loop_start = 0; // the latest of the looping channels' loop points and the others' ends, or a loop later
    uint32_t loop_end = 0;   // the loop start plus the length LoopLength() gives for the channels' loops
    uint32_t next_end = 0;   // as far again: the end of the song's next pass through its loop
    uint32_t end = 0;
};

// A channel's loop point, the ends of its passes through its loop, and where it ends or stops at its loop point. Each
// is an index into the events. Each is -1 or empty until the channel gets there.
struct ChannelLoop
{
    // Returns true if the channel has been through its loop.
    bool Loops() const
    {
        return !passes.empty() && loop_point >= 0;
    }

    int loop_point = -1;
    std::vector<int> passes;
    int end = -1;
    bool idle = false; // a sound effect leaves the channel to the music
};

// The model's output for the stretch of a song that the plan covers, and its timing in the MIDI file.
struct Simulation
{
    std::vector<FrameData> frames;
    std::vector<Event> events;
    std::vector<uint32_t> frame_ticks; // the MIDI tick each frame starts at, and the one after the last frame
    std::array<ChannelLoop, kChannels> channel_loops;
    int loop_channel = -1;             // the channel with the longest loop, or -1 if no channel loops
    size_t times = 1;                  // the loop channel's passes through its loop in each pass of the song's loop
    std::vector<uint32_t> event_ticks; // each event's MIDI tick
    Plan plan;
    std::vector<std::string> warnings;
    bool cut_off = false;                // the model stopped at kMaxFrames
    std::vector<TrackStretch> stretches; // while the model records a performance (Sequencer::RecordPerformance)
};

// A change that the frequency sweep makes to a square's pitch within a frame.
struct SweepStep
{
    double at = 0;     // its position in the frame, from 0 to 1
    uint32_t tick = 0; // its MIDI tick
    double bend = 0;   // semitones from the note's key
};

// A channel's sound in one frame.
struct Sound
{
    bool sounding = false;
    double left = 0, right = 0; // levels, with a full-scale FIFO sample at 128
    double bend = 0;            // semitones from the note's key
    int shape = -1;             // the duty or the wave shape that the frame plays
    uint32_t tick = 0;          // the MIDI tick its changes go at
    std::vector<SweepStep> sweeps;
    // What the driver writes for a square: the envelope register (NRx2), with 0x100 in a frame that restarts the
    // channel, the sweep register (NR10) on square 1, and the scale of each side. The hardware's envelope changes the
    // level of a square on a clock that isn't tied to the frames. On the other channels, `left` and `right` change only
    // with the driver's writes.
    int envelope = -1;
    int sweep = -1;
    double side_left = 0, side_right = 0;
};

// The SoundFont instrument a note plays.
struct Instrument
{
    enum Kind : uint8_t
    {
        kSquare,
        kWave,
        kNoiseKit,
        kPcm,
    };

    bool operator<(const Instrument& o) const
    {
        return std::tie(kind, first, frames, second, pcm) < std::tie(o.kind, o.first, o.frames, o.second, o.pcm);
    }

    Kind kind = kSquare;
    int first = 0;  // the duty or the wave shape at a note's start
    int frames = 0; // the frames before it switches, or 0
    int second = 0; // the duty or the wave shape it switches to
    PcmSound pcm;
};

// A note as the MIDI file plays it.
struct Note
{
    int key = 60;
    int program = 0;
    uint32_t on = 0; // MIDI ticks
    uint32_t off = 0;
    uint32_t command = 0;      // the address of the note or the "play again" that started it
    std::vector<Sound> sounds; // one for each of its frames
    NoiseSound noise;          // a noise note's drum
    PcmSound pcm;              // a PCM note's sample
};

// Converts a song's channels into the MIDI file's notes.
class NoteMaker
{
public:
    NoteMaker(const Rom& rom, const DriverInfo& info, const Simulation& sim);

    // Returns channel `c`'s notes, and adds the instruments they play.
    std::vector<Note> Make(int c);

    // Returns the instruments the notes play, with their programs.
    const std::map<Instrument, int>& Programs() const
    {
        return programs_;
    }

    // Returns the noise kit's drums, with their keys.
    const std::map<NoiseSound, int>& Drums() const
    {
        return drums_;
    }

    // Returns the wave shapes that the notes play, by number.
    const WaveShapes& Shapes() const
    {
        return shapes_;
    }

private:
    // A stretch of a channel from a note or "play again" to the next note, rest or end.
    struct Stretch
    {
        uint32_t first = 0; // the frame it starts in
        uint32_t last = 0;  // the frame the next stretch starts in
        uint32_t on = 0;    // its start and end in MIDI ticks
        uint32_t stop = 0;
        int key = 60;
        int base = 0;         // the frequency setting or PCM pitch that the key stands for
        uint32_t command = 0; // the address of the note or the "play again"
    };

    // Adds the notes of a stretch of channel `c`: one for each run of its frames in which the channel sounds. Each
    // frame's changes go as far into it as the stretch's start is into its first frame. They stay before the next
    // stretch. `psg` is the model of a PSG channel. It's null for a PCM channel.
    void AddStretch(int c, const Stretch& stretch, PsgFeed* psg, std::vector<Note>& notes);

    // Returns a note's MIDI key, and sets `base` to the pitch it stands for.
    int KeyOf(int c, int index, int& base) const;

    // Returns a PSG channel's sound in a frame. `volume` is the volume that the channel's model gives for the frame's
    // start, or -1 if the channel is off. `psg` contains the frequency and the sweep's changes for the frame.
    Sound PsgSound(int c, int volume, const PsgModel& psg, const FrameData& d, int base);

    // Returns a PCM channel's sound in a frame.
    Sound FifoSound(int c, const FrameData& d, int base) const;

    // Returns what a FIFO plays from its last start, at the rate it plays at then: the frames until the driver stops it
    // or loops it, and the frames of each pass through the loop as the bytes the FIFO plays in them. A FIFO that
    // started from its loop plays passes through the loop from the start.
    static PcmSound PcmSoundOf(const Fifo& fifo);

    // Gives a finished note its instrument and keeps it.
    void Finish(int c, Note& note, std::vector<Note>& notes);

    const Rom& rom_;
    const DriverInfo& info_;
    const Simulation& sim_;
    const uint32_t end_;
    std::map<Instrument, int> programs_;
    std::map<NoiseSound, int> drums_;
    WaveShapes shapes_;
};

} // namespace
} // namespace agblib::sequence_export::quintet
