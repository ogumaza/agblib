// SPDX-License-Identifier: MIT

// Definitions that the parts of the Ubisoft Milan sequencer share (sequencer.cpp and sequencer_psg.cpp): the sound
// registers, a sample's and an instrument's fields, and the place of a key in the PSG's frequency table. The two files
// build as one translation unit. Each file also compiles on its own. The definitions keep internal linkage. Only those
// files include this header.

#pragma once

#include <array>
#include <cstdint>

#include "agblib/agb/gba.h"
#include "agblib/agb/sound_registers.h"
#include "agblib/agb/warnings.h"
#include "agblib/ubimilan/driver.h"

namespace agblib::ubimilan
{

using agblib::agb::kFrameCycles;
using agblib::agb::WarnOnce;

namespace
{

// What the game's start writes to SOUNDBIAS.
constexpr uint32_t kBiosBias = 0x0200;
constexpr uint32_t kBiasResolution = 0x4000;

// A channel's frequency register.
constexpr std::array<uint32_t, kPsgChannels> kFrequencyRegisters = {agb::kRegNr13, agb::kRegNr23, agb::kRegNr33};

// The highest volume that the game's sound update allows a voice.
constexpr uint32_t kMostVolume = 0x40;

// A sample resource's fields, from the resource's address.
constexpr uint32_t kSampleLoops = 1;
constexpr uint32_t kSampleLength = 4;
constexpr uint32_t kSampleLoopStart = 8;
constexpr uint32_t kSampleLoopEnd = 12;
constexpr uint32_t kSampleData = 16;

// The kinds that the game's sound update takes for the sequence after the next.
constexpr uint8_t kSoundUpdateKeeps = kSample;

// An instrument's fields, from its 16 bytes.
constexpr uint32_t kModulationDelay = 4;
constexpr uint32_t kModulationDepth = 6;
constexpr uint32_t kModulationPeriod = 8;
constexpr uint32_t kSlideFrames = 10;
constexpr uint32_t kInstrumentFlags = 12;

// An instrument's flags: modulation, a pitch mode, and a note that doesn't start the channel again.
constexpr uint16_t kModulation = 1;
constexpr uint16_t kPitchMode = 2;
constexpr uint16_t kLegato = 4;

// Returns the byte that gives a key's place in the PSG's frequency table: 36 keys down on the squares and 12 on the
// wave channel.
int KeyIndex(int channel, int key)
{
    if (channel == kWaveChannel)
    {
        return (key - 12) & 0xFF;
    }

    return (key - 36) & 0xFF;
}

} // namespace

} // namespace agblib::ubimilan
