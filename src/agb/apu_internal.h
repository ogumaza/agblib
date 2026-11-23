// SPDX-License-Identifier: MIT

// What the files of the model of the GBA's sound hardware share: the registers' offsets and bits, and the timers'
// periods and steps. apu.cpp and apu_output.cpp build as one translation
// unit, in that order. The definitions keep internal linkage. The functions then compile as they did in one file.

#pragma once

#include <cstdint>

#include "agblib/agb/apu.h"
#include "agblib/agb/gba.h"

namespace agblib::agb
{
namespace
{

// The registers' offsets from kRegSound1CntL.
constexpr uint32_t kNr10 = 0x00;
constexpr uint32_t kNr11 = 0x02;
constexpr uint32_t kNr12 = 0x03;
constexpr uint32_t kNr13 = 0x04;
constexpr uint32_t kNr14 = 0x05;
constexpr uint32_t kNr21 = 0x08;
constexpr uint32_t kNr22 = 0x09;
constexpr uint32_t kNr23 = 0x0C;
constexpr uint32_t kNr24 = 0x0D;
constexpr uint32_t kNr30 = 0x10;
constexpr uint32_t kNr31 = 0x12;
constexpr uint32_t kNr32 = 0x13;
constexpr uint32_t kNr33 = 0x14;
constexpr uint32_t kNr34 = 0x15;
constexpr uint32_t kNr41 = 0x18;
constexpr uint32_t kNr42 = 0x19;
constexpr uint32_t kNr43 = 0x1C;
constexpr uint32_t kNr44 = 0x1D;
constexpr uint32_t kNr50 = 0x20;
constexpr uint32_t kNr51 = 0x21;
constexpr uint32_t kSoundCntH = 0x22;
constexpr uint32_t kSoundCntX = 0x24;
constexpr uint32_t kSoundBias = 0x28;
constexpr uint32_t kWaveRam = 0x30;

// The registers that turning the sound off clears: NR10 to NR51. The Game Boy clears the same registers.
constexpr uint32_t kPsgRegisters = 0x22;

// The bits of the frequency registers' high bytes.
constexpr uint8_t kLengthEnable = 0x40;
constexpr uint8_t kRestart = 0x80;

// SOUNDCNT_X's bit that turns the sound on.
constexpr uint8_t kMasterEnable = 0x80;

// The squares' duties, the first step in bit 7: 12.5%, 25%, 50% and 75%.
constexpr uint8_t kDuties[4] = {0x01, 0x81, 0x87, 0x7E};

// The cycles of a step of the squares' and the wave's timers, for each step of 2048 less the frequency.
constexpr uint32_t kSquareStepCycles = 16;
constexpr uint32_t kWaveStepCycles = 8;

// The noise's clock shifts from 14 on stop its counter.
constexpr int kNoiseShifts = 14;

uint32_t SquarePeriod(uint16_t frequency)
{
    return (2048u - frequency) * kSquareStepCycles;
}

uint32_t WavePeriod(uint16_t frequency)
{
    return (2048u - frequency) * kWaveStepCycles;
}

// The noise's frequency is 524,288 Hz / r / 2^(s+1), with r = 0.5 for a ratio of 0.
uint32_t NoisePeriod(uint8_t nr43)
{
    const uint32_t ratio = nr43 & 7u;
    const uint32_t shift = nr43 >> 4;
    return ratio ? ratio << (shift + 6) : 1u << (shift + 5);
}

} // namespace
} // namespace agblib::agb
