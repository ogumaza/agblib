// SPDX-License-Identifier: MIT

// The PSG's waveforms as the R&D drivers' SoundFonts sample them: a square channel's cycle without its DC offset, a
// wave channel's pattern, and the noise channel's output at a fixed rate. `share` is the PSG channel's full level
// relative to a full-scale sample.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/sequence_export/agb/psg.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::agb
{

// The high steps of each duty cycle, of 8.
inline constexpr int kDutyHighSteps[4] = {1, 2, 4, 6};

// Returns a cycle of a square channel's waveform at duty `duty` (0-3): 8 steps of `step_points` points, the high steps
// first. The GBA's output removes the DC offset. Without it, the levels sit either side of 0. They are as far apart as
// a wave's levels 0 and 15.
inline std::vector<int16_t> CenteredSquareCycle(int duty, int step_points, double share)
{
    const int high = kDutyHighSteps[duty & 3];
    std::vector<int16_t> cycle;
    for (int step = 0; step < 8; step++)
    {
        const double level = step < high ? 2.0 - high / 4.0 : -high / 4.0;
        for (int i = 0; i < step_points; i++)
        {
            cycle.push_back(int16_t(std::lround(level * share * 32767)));
        }
    }

    return cycle;
}

// Returns the wave channel's pattern at `address`: 32 steps of 4 bits, the high nibble of each byte first.
inline std::vector<int16_t> WavePatternCycle(const agblib::agb::Rom& rom, uint32_t address, double share)
{
    std::vector<int16_t> cycle;
    for (uint32_t i = 0; i < 16; i++)
    {
        const uint8_t b = rom.U8(address + i);
        for (int v : {b >> 4, b & 15})
        {
            cycle.push_back(int16_t(std::lround((v - 7.5) / 7.5 * share * 32767)));
        }
    }

    return cycle;
}

// Appends `points` points of the noise channel's output at NR43 setting `nr43` to `sample`. The LFSR's clock is
// 524288 / r / 2^(s+1) Hz, with r = 0 counting as 0.5. Each point moves it on by `step` / ((r ? 2r : 1) << s) steps.
// The output is high while the LFSR's low bit is 0. A shift of 14 or 15 stops the clock, and the output is silent.
inline void AppendNoise(sf2::Sample& sample, uint8_t nr43, uint32_t points, uint32_t step, double share)
{
    const int shift = nr43 >> 4;
    const int ratio = nr43 & 7;
    uint32_t lfsr = 0x7FFF;
    uint32_t clock = 0;
    for (uint32_t i = 0; i < points; i++)
    {
        sample.data.push_back(shift < 14 ? int16_t(std::lround(((lfsr & 1) ? -32767 : 32767) * share)) : 0);
        if (shift >= 14)
        {
            continue;
        }

        clock += step;
        const uint32_t den = uint32_t(ratio ? 2 * ratio : 1) << shift;
        for (; clock >= den; clock -= den)
        {
            lfsr = NextLfsr(lfsr, (nr43 & 8) != 0);
        }
    }
}

} // namespace agblib::sequence_export::agb
