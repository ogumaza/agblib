// SPDX-License-Identifier: MIT

// The SoundFont of the samples that a song's notes play.

#pragma once

#include <compare>
#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/frontline_mod/driver.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::frontline_mod
{

using agblib::agb::Rom;
using agblib::frontline_mod::DriverInfo;
using agblib::sequence_export::agb::ModuleInstruments;
using agblib::sequence_export::agb::Sf2File;

// The MIDI key of a SoundFont sample's rate.
constexpr int kRootKey = 60;

// The period index whose step plays a sample at its SoundFont rate. Each index is 1/64 of a semitone, and this one
// plays 8,363 points a second: the middle C of MOD files.
constexpr uint32_t kRootIndex = 4608;

// A sample as a note plays it: the address of its points, its length, the instrument's loop, the rate at which the
// root index plays it, and the point that the note starts from.
struct PlayedSample
{
    auto operator<=>(const PlayedSample&) const = default;

    uint32_t points = 0;
    uint32_t length = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0; // 0 for no loop
    uint32_t rate = 0;     // Hz
    uint32_t offset = 0;   // the point of the SoundFont sample that the note starts from
};

// Returns the rate in hertz at which the mixer plays a sample at the root index of the table of periods in `info`.
double RootRate(const Rom& rom, const DriverInfo& info);

// Returns true if the mixer loops `sample`.
bool Loops(const PlayedSample& sample);

// Each instrument is a sample played from a given point. Instruments become SoundFont presets, numbered in order of
// first use. A shared SoundFont can use one set for all the game's songs.
//
// The mixer plays a loop up to and including the point at the loop's end. That point can be the first byte after the
// sample. The SoundFont sample therefore ends with that point, and its loop includes it. A sample without a loop stays
// as it is. The zones keep the default release, the shortest that the format allows: the mixer stops a channel at once.
class InstrumentSet
{
public:
    // Returns the program of `sample`, instrument `number` in its module, adding it if it's new. Programs from 128 on
    // go in the banks after the first.
    int Program(const PlayedSample& sample, int number);

    // Returns the SoundFont of the instruments.
    Sf2File Build(const Rom& rom) const;

private:
    ModuleInstruments<PlayedSample> instruments_;
};

} // namespace agblib::sequence_export::frontline_mod
