// SPDX-License-Identifier: MIT

// Definitions that the parts of the Konami conversion share (convert.cpp, convert_tracks.h and convert_presets.h): the
// names they use, the song's simulation, the events of its tracks, the layouts of its presets, and the tracks' names.
// Only those files include this header. The definitions therefore keep internal linkage.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/files.h"
#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/konami/driver.h"
#include "agblib/konami/seqformat.h"
#include "agblib/konami/sequencer.h"
#include "agblib/sequence_export/agb/beat_grid.h"
#include "agblib/sequence_export/agb/beat_midi.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/psg.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/agb/sfz.h"
#include "agblib/sequence_export/agb/song_banks.h"
#include "agblib/sequence_export/konami/soundfont.h"

namespace agblib::sequence_export::konami
{

using agblib::agb::FrameSeconds;
using agblib::agb::kMaxSilentEndSeconds;
using agblib::agb::LoopStart;
using agblib::agb::PassComparison;
using agblib::agb::WriteFile;
using agblib::agb::WriteTextFile;
using agblib::konami::Command;
using agblib::konami::IsPsgTrack;
using agblib::konami::kC2;
using agblib::konami::kDungeonDiceNoiseNote;
using agblib::konami::kLiveStageSetting;
using agblib::konami::kMaxFrames;
using agblib::konami::kMaxWalkCommands;
using agblib::konami::kOutDungeonDiceNote;
using agblib::konami::kOutDungeonDicePeriod;
using agblib::konami::kOutNoteOn;
using agblib::konami::kOutPan;
using agblib::konami::kOutPitch;
using agblib::konami::kOutStop;
using agblib::konami::kPsgTracks;
using agblib::konami::kTracks;
using agblib::konami::Op;
using agblib::konami::Request;
using agblib::konami::Revision;
using agblib::konami::SampleInfo;
using agblib::konami::Sequencer;
using agblib::konami::SongHeader;
using agblib::konami::TableEntry;
using agblib::konami::TrackOutput;
using agblib::konami::TrackRead;
using agblib::sequence_export::agb::BankProgram;
using agblib::sequence_export::agb::BeatGrid;
using agblib::sequence_export::agb::BeatTicks;
using agblib::sequence_export::agb::FixedBendValue;
using agblib::sequence_export::agb::kBeatQuarterTicks;
using agblib::sequence_export::agb::kDrumBank;
using agblib::sequence_export::agb::kDrumChannel;
using agblib::sequence_export::agb::kPsgLevelPerVolume;
using agblib::sequence_export::agb::LevelsToControllers;
using agblib::sequence_export::agb::MakeInstrument;
using agblib::sequence_export::agb::MakePreset;
using agblib::sequence_export::agb::MidiFile;
using agblib::sequence_export::agb::MidiTrack;
using agblib::sequence_export::agb::QuarterTick;
using agblib::sequence_export::agb::RootKeyZone;
using agblib::sequence_export::agb::SfzTrack;
using agblib::sequence_export::agb::SongNumber;
using agblib::sequence_export::agb::SquareHz;
using agblib::sequence_export::agb::SquareName;
using agblib::sequence_export::agb::TunedZones;
using agblib::sequence_export::agb::WaveShare;
namespace cc = seqlib::midi::cc;

namespace
{

// Shared settings for one frame: echo, plus PSG master volume and wave RAM selection in Dungeon Dice Monsters.
struct FrameState
{
    uint16_t bus0 = 0, bus1 = 0;
    uint8_t feedback = 0, delay = 0;
    uint8_t psg_volume = 0x77; // NR50
    int wave = 0;
    int wave_loads = 0; // wave loads since the song started, including reloads
};

// Every frame of a song's sequencer output, and the frames of the marked pass through its loop.
struct Simulation
{
    std::vector<std::array<TrackOutput, kTracks>> frames;
    std::vector<FrameState> state;
    int loop_start = -1, loop_end = -1; // frames of the marked pass through the loop
    int first_loop_end = -1;            // the frame the song first loops in: its second pass starts there
    bool delayed_loop_point = false;    // a track's loop point starts with a delay that only the later passes play
    bool truncated = false;             // the song reached the frame limit
    LoopStart start_pass = LoopStart::kFirstPass;
    std::vector<std::string> warnings;
};

// A track event for MIDI conversion.
struct Event
{
    // The kinds of event.
    enum Type
    {
        kNoteOn,
        kNoteOff,
        kLevels,
        kBend,
        kEcho
    };

    Type type;
    uint32_t frame;
    Kind kind = Kind::kSample;
    int param = 0; // duty, wave row (wave << 4 | volume), noise note or sample index
    int note = 0;  // PSG note number or DS semitone offset
    int dev = 0;   // pitch deviation from the note, 1/32 semitones
    double level_l = 0, level_r = 0;
    int echo = 0; // reverb send standing in for the driver's echo
};

// Mixer level of a DS voice / PSG channel on each side, in DirectSound units: a level of n plays a full-scale sample at
// amplitude n/128.
struct Levels
{
    double left = 0, right = 0;
};

// Preset and key assigned to a note on a sample or noise track.
struct Placement
{
    int preset = 0; // index into the preset list
    int key = 60;
};

// A key range of a preset, and the sample it plays.
struct KeyZone
{
    int sample = -1; // SF2 sample
    int lo = 0, hi = 127;
    int root = 60;
    bool loop = false;
    bool melodic = false;
};

// Key layout of one DS track: the preset and key that play each (sample, semitone).
struct TrackKeymap
{
    std::vector<std::vector<KeyZone>> presets;
    std::map<std::pair<int, int>, Placement> notes;
};

// Sound sources, pitches and timing used to build the song's SoundFont and MIDI file.
struct Usage
{
    std::array<bool, kTracks> used{};                          // tracks that play notes
    std::array<int, kTracks> max_dev{};                        // largest pitch deviation, 1/32 semitones
    std::array<std::map<int, std::set<int>>, kTracks> samples; // DS sample -> the semitones it's played at
    std::set<int> duties, waves, noise_notes;
};

// The locations of a song's instruments in the SoundFont.
struct Programs
{
    std::map<int, BankProgram> duty;     // by duty cycle
    std::map<int, BankProgram> wave;     // by wave row, wave << 4 | volume
    std::vector<BankProgram> noise;      // the noise presets
    std::map<int, Placement> noise_keys; // noise note -> its preset and key
    std::array<TrackKeymap, kTracks> keymaps;
    std::array<std::vector<BankProgram>, kTracks> tracks; // the presets of each DS track
    bool fits = true;                                     // every preset found a free program
};

// Preset and MIDI key for a note.
struct NoteSound
{
    BankProgram program;
    int key = 0;
};

const char* const kPsgNames[kPsgTracks] = {"Square 1", "Square 2", "Wave", "Noise"};

std::string TrackName(int t)
{
    if (IsPsgTrack(t))
    {
        return kPsgNames[t];
    }

    return "Voice " + std::to_string(t - kPsgTracks);
}

} // namespace
} // namespace agblib::sequence_export::konami
