// SPDX-License-Identifier: MIT

// The SoundFont of the samples that a song's notes play.

#pragma once

#include <compare>
#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::engine_software
{

using agblib::agb::Rom;
using agblib::engine_software::DriverInfo;
using agblib::sequence_export::agb::ModuleInstruments;
using agblib::sequence_export::agb::Sf2File;

// The MIDI key of a SoundFont sample's rate: the driver's note 49 with the instrument's relative note and fine tune.
constexpr int kRootKey = 60;

// A sample as a note plays it: the address of its points, its length, its loop, the rate at which the root note plays
// it, and the point that the note starts from. Lengths and loop points count whole points.
struct PlayedSample
{
    auto operator<=>(const PlayedSample&) const = default;

    uint32_t points = 0;
    uint32_t length = 0;
    uint32_t loop_start = 0;
    bool loops = false;
    uint32_t rate = 0;   // Hz
    uint32_t offset = 0; // the point of the SoundFont sample that the note starts from
};

// Returns the rate in hertz at which the mixer plays the root note of the instrument whose record is at `record`: note
// 49 with the instrument's relative note and fine tune.
double RootRate(const Rom& rom, const DriverInfo& info, uint32_t record);

// Each instrument is a sample played from a given point. Instruments become SoundFont presets, numbered in order of
// first use. A shared SoundFont can use one set for all the game's songs.
//
// The mixer plays a loop from its start to its end and keeps the part of a step past the end in the loop. The
// SoundFont sample therefore ends with the loop. A sample that plays once stays as it is. The zones keep the default
// release, the shortest that the format allows: the mixer stops a channel at once. The volume envelopes and the
// fade-outs are in the notes' levels, not in the zones.
class InstrumentSet
{
public:
    // Returns the program of `sample`, instrument `number` in the bank, adding it if it's new. Programs from 128 on go
    // in the banks after the first.
    int Program(const PlayedSample& sample, int number);

    // Returns the SoundFont of the instruments.
    Sf2File Build(const Rom& rom) const;

private:
    ModuleInstruments<PlayedSample> instruments_;
};

} // namespace agblib::sequence_export::engine_software
