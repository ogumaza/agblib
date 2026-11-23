// SPDX-License-Identifier: MIT

// The SoundFont of the samples that a song's notes play.

#pragma once

#include <compare>
#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::frontline_xm
{

using agblib::agb::Rom;
using agblib::frontline_xm::DriverInfo;
using agblib::sequence_export::agb::ModuleInstruments;
using agblib::sequence_export::agb::Sf2File;

// The MIDI key of a SoundFont sample's rate, and the period that plays it: note 48. It plays a sample without a
// finetune at 8,363 points a second, the middle C of XM files.
constexpr int kRootKey = 60;
constexpr uint32_t kRootPeriod = 4608;

// A sample as a note plays it: the address of its points, its length, the instrument's loop, the rate at which the root
// period plays it with the instrument's finetune, and the point that the note starts from. Lengths and loop points
// count whole points.
struct PlayedSample
{
    auto operator<=>(const PlayedSample&) const = default;

    uint32_t points = 0;
    uint32_t length = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    bool loops = false;
    uint32_t rate = 0;   // Hz
    uint32_t offset = 0; // the point of the SoundFont sample that the note starts from
};

// Returns the rate in hertz at which the mixer plays a sample at the root period of an instrument with `finetune`.
double RootRate(const Rom& rom, const DriverInfo& info, int8_t finetune);

// Each instrument is a sample played from a given point. Instruments become SoundFont presets, numbered in order of
// first use. A shared SoundFont can use one set for all the game's songs.
//
// The mixer plays a loop from its start up to the point before its end, and keeps the fraction of a position past the
// end to the loop's start. The SoundFont sample therefore ends with the loop. A sample without a loop stays as it is.
// The zones keep the default release, the shortest that the format allows: the mixer stops a channel at once.
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

} // namespace agblib::sequence_export::frontline_xm
