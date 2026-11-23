// SPDX-License-Identifier: MIT

// Definitions that the parts of the Krawall conversion share (convert.cpp and convert_simulation.h): the names they
// use, and the simulation of a module, its notes and timing. Only those files include this header. The definitions
// therefore keep internal linkage.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/files.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/krawall/driver.h"
#include "agblib/krawall/player.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/module_bars.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/krawall/convert.h"
#include "agblib/sequence_export/krawall/soundfont.h"

namespace agblib::sequence_export::krawall
{

using agblib::agb::LoopStart;
using agblib::agb::PassComparison;
using agblib::krawall::Channel;
using agblib::krawall::kPlayerChannels;
using agblib::krawall::MixChannel;
using agblib::krawall::ModuleSong;
using agblib::krawall::Player;
using agblib::sequence_export::agb::BarStarts;
using agblib::sequence_export::agb::BarTicks;
using agblib::sequence_export::agb::BendRangeFor;
using agblib::sequence_export::agb::ChannelWriter;
using agblib::sequence_export::agb::ChooseMeter;
using agblib::sequence_export::agb::kSmallBend;
using agblib::sequence_export::agb::LevelsToControllers;
using agblib::sequence_export::agb::MelodicChannel;
using agblib::sequence_export::agb::Meter;
using agblib::sequence_export::agb::MidiFile;
using agblib::sequence_export::agb::MidiTrack;
using agblib::sequence_export::agb::ModuleStep;
using agblib::sequence_export::agb::NumberedPreset;
using agblib::sequence_export::agb::OrderRun;
using agblib::sequence_export::agb::OrderRuns;
using agblib::sequence_export::agb::SongNumber;
using agblib::sequence_export::agb::SongStem;

namespace
{

// A channel's mixer channel after a tick.
struct Voice
{
    bool playing = false;
    uint32_t handle = 0;
    uint32_t moves = 0;
    uint32_t sample = 0;
    int sample_index = -1;
    uint32_t offset = 0; // the point it plays, from the sample's start
    double rate = 0;     // Hz
    double left = 0;     // the level of each side, where 128 is full scale
    double right = 0;
    uint32_t entry = 0; // the address of the byte of flags of the channel's last entry in a row
};

// What the conversion keeps of a tick.
struct TickData
{
    uint64_t time = 0;     // the mixer's samples from the module's first tick
    uint32_t length = 0;   // the samples to the next tick
    bool row = false;      // a row starts on the tick
    int order = 0;         // the row's order
    int pattern_row = 0;   // the row's number in its pattern
    int speed = 0;         // the ticks of its row, at least 1
    uint32_t row_data = 0; // the row's first byte, and the address after the 0 that ends it
    uint32_t row_end = 0;
    std::array<Voice, kPlayerChannels> voices;
    std::array<std::pair<int8_t, int8_t>, kPlayerChannels> levels; // each channel's volume and pan
};

// The stretch of a module that the conversion covers, in ticks.
struct Plan
{
    bool loops = false;
    size_t loop_start = 0; // the first row whose place in the module comes back
    size_t loop_end = 0;   // where it comes back
    size_t end = 0;
};

// The model's output for the stretch of a module that the plan covers.
struct Simulation
{
    std::vector<TickData> ticks; // up to and including the plan's end
    Plan plan;
    int channels = 0;
    int mix_rate = 0;
    bool truncated = false; // cut off without a loop or a stop
    std::vector<std::string> warnings;
};

// A tick of a note: how far its pitch is from the note's key, in semitones, and its levels.
struct Sound
{
    size_t tick = 0;
    double bend = 0;
    double left = 0;
    double right = 0;
};

// A note: a sample that a channel's mixer channel plays from the tick a note or an effect starts it until it stops,
// another starts, or the module ends.
struct Note
{
    size_t on = 0;
    size_t off = 0;
    int key = 0;
    int program = 0;
    uint32_t entry = 0; // the address of the byte of flags of the entry that started it
    std::vector<Sound> sounds;
};

// MIDI timing based on the most common speed. At that speed, and at power-of-two multiples or fractions of it, each
// player tick occupies the same number of MIDI ticks. This lets a module halve its speed for finer rows without
// changing the MIDI tempo. At other speeds, each row gets the usual number of MIDI ticks, and the tempo changes to
// preserve its playback duration, subject to the resolution limit below.
struct Timing
{
    Meter meter;                                       // in MIDI ticks
    std::vector<uint32_t> ticks;                       // the MIDI tick of each of the module's ticks, and of the end
    std::vector<std::pair<uint32_t, uint32_t>> tempos; // MIDI tick and microseconds a quarter note
    std::vector<uint32_t> bars;                        // the MIDI ticks that start a bar
};

} // namespace
} // namespace agblib::sequence_export::krawall
