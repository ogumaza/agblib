// SPDX-License-Identifier: MIT

// The SoundFont of the samples that a song's notes play.

#pragma once

#include <compare>
#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/webfoot/driver.h"

namespace agblib::sequence_export::webfoot
{

using agblib::agb::Rom;
using agblib::sequence_export::agb::ModuleInstruments;
using agblib::sequence_export::agb::Sf2File;
using agblib::webfoot::Mixer;

// The MIDI key that a SoundFont sample plays at its rate: the driver's middle note, Impulse Tracker's C-5.
constexpr int kRootKey = 60;

// A sample as a note plays it: its entry's address, points, length, loop and flags, the rate at which the middle note
// plays it, and the point that the note starts from.
struct PlayedSample
{
    auto operator<=>(const PlayedSample&) const = default;

    uint32_t entry = 0;
    uint32_t points = 0;
    uint16_t length = 0;
    uint16_t loop_start = 0;
    uint16_t flags = 0;
    uint32_t rate = 0;   // Hz
    uint32_t offset = 0; // the point of the SoundFont sample that the note starts from
};

// Returns the rate in hertz at which the mixer plays a sample of rate `rate` at the middle note. The driver works the
// step out for a nominal rate, and the timer runs a little off it. The SoundFont sample has this rate. The difference
// of about 2 cents is then part of the sample.
double PlayedRate(uint16_t rate, const Mixer& mixer);

// Returns true if the mixer plays the loop of `sample` as a ping-pong loop.
bool PingPong(const PlayedSample& sample);

// Each instrument is a sample played from a given point. Instruments become SoundFont presets, numbered in order of
// first use. A shared SoundFont can use one set for all the game's songs.
//
// A forward loop stays as it is. A ping-pong loop becomes the sample's points to its end, then the loop's points
// backwards, and the SoundFont loops over both. The zones keep the default release, the shortest that the format
// allows: the driver stops a voice at once.
class InstrumentSet
{
public:
    // Returns the program of `sample`, number `number` from 0 in its sample table, adding it if it's new. Programs from
    // 128 on go in the banks after the first.
    int Program(const PlayedSample& sample, int number);

    // Returns the SoundFont of the instruments.
    Sf2File Build(const Rom& rom) const;

private:
    ModuleInstruments<PlayedSample> instruments_;
};

} // namespace agblib::sequence_export::webfoot
