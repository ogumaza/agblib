// SPDX-License-Identifier: MIT

// What the tests of the model of the GBA's sound hardware share: the output of a PSG level, and a model with the sound
// on. The sound registers have the names of agb/sound_registers.h.

#pragma once

#include <cstdint>

#include "agblib/agb/apu.h"
#include "agblib/agb/gba.h"
#include "agblib/agb/sound_registers.h"

namespace agblib::agb::apu_test
{

// The cycles of an output sample at the default resolution's 32,768 Hz.
constexpr uint32_t kSampleCycles = 512;

// The output of one PSG level with the master volume at 7 and the PSG at 100%: 2 * 8 steps of 64.
constexpr int kLevel = 1024;

// The sound on, both master volumes at 7, every PSG channel on both sides and the PSG at 100%.
inline Apu PoweredOn()
{
    Apu apu;
    apu.Store(kRegSoundCntX, 1, 0x80);
    apu.Store(kRegNr50, 1, 0x77);
    apu.Store(kRegNr51, 1, 0xFF);
    apu.Store(kRegSoundCntH, 2, 0x0002);
    return apu;
}

} // namespace agblib::agb::apu_test
