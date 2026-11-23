// SPDX-License-Identifier: MIT

// The steps that the GBA drivers' archive players share: the checks of a call's state, the runs of the sound hardware
// for a part of a frame, the masks of the muted and the soloed tracks, and the peaks of the meters.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "agblib/agb/apu.h"

namespace agblib::agb
{

// Throws std::logic_error unless a `holder`, such as a song, has started.
inline void CheckStarted(bool started, const char* holder = "song")
{
    if (!started)
    {
        throw std::logic_error(std::string("no ") + holder + " has started");
    }
}

// Throws std::invalid_argument unless a snapshot is `owned` by the player that restores it.
inline void CheckSnapshotOwner(bool owned)
{
    if (!owned)
    {
        throw std::invalid_argument("the snapshot is another player's");
    }
}

// Throws std::logic_error when a frame of 16-bit samples is asked of a `holder`, such as a song, that plays in the
// enhanced mode.
inline void CheckPlainFrame(bool enhanced, const char* holder = "song")
{
    if (enhanced)
    {
        throw std::logic_error(std::string("the ") + holder + " plays in the enhanced mode");
    }
}

// Throws std::logic_error when a frame of the enhanced mode's samples is asked of a `holder`, such as a song, that has
// started and doesn't play in that mode.
inline void CheckEnhancedFrame(bool started, bool enhanced, const char* holder = "song")
{
    if (started && !enhanced)
    {
        throw std::logic_error(std::string("the ") + holder + " doesn't play in the enhanced mode");
    }
}

// Runs `apu` for `cycles` cycles: into `out`, or, where `out` is null, into `enhanced_out`, or without samples where
// both are null.
inline void RunApu(Apu& apu, uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (out)
    {
        apu.Run(cycles, *out);
    }
    else if (enhanced_out)
    {
        apu.Run(cycles, *enhanced_out);
    }
    else
    {
        apu.Run(cycles);
    }
}

// Runs `apu` for `cycles` cycles as RunApu() does, and makes the FIFO timers' changes of `changes`, from index `next`
// on as their cycles come. A change has the fields `cycle`, `timer`, `period` and `first`, and comes once the
// overflows before it have taken their samples. `mixed` counts the cycles that ran since the changes' frame began.
template <typename Change>
void RunApuWithChanges(Apu& apu, const std::vector<Change>& changes, size_t& next, uint64_t& mixed, uint64_t cycles,
                       std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    while (next < changes.size() && changes[next].cycle <= mixed + cycles)
    {
        const Change& change = changes[next++];
        const uint64_t part = change.cycle - mixed;
        RunApu(apu, part, out, enhanced_out);
        cycles -= part;
        mixed = change.cycle;
        apu.SetTimer(change.timer, change.period, change.first);
    }

    RunApu(apu, cycles, out, enhanced_out);
    mixed += cycles;
}

// The tracks that play silent: those of `muted`, and while any track is soloed, every track that `soloed` leaves out.
template <typename Mask>
Mask SilentTracksOf(Mask muted, Mask soloed)
{
    return Mask(muted | (soloed ? ~soloed : 0));
}

// Sets bit `track` of `mask` when `on`, and clears it otherwise. Throws std::out_of_range unless `track` is one of
// `tracks` tracks, with a message that a `holder`, such as a song, has at most that many.
template <typename Mask>
void SetTrackBit(Mask& mask, int track, bool on, int tracks, const char* holder = "song")
{
    if (track < 0 || track >= tracks)
    {
        throw std::out_of_range(std::string("a ") + holder + " has at most " + std::to_string(tracks) + " tracks");
    }

    mask = Mask(on ? mask | 1u << track : mask & ~(1u << track));
}

// Raises `peak`, the left side's first, to a voice's parts of the FIFOs' samples, `parts`, FIFO A's first, on the scale
// of 16-bit samples. A FIFO's byte counts 4 or 2 times in the output's 10 bits: the FIFO's volume at 100% or 50% in
// SOUNDCNT_H's low byte, `volumes`. SOUNDCNT_H's high byte, `sides`, puts each FIFO on its sides, the right in the low
// bit.
inline void KeepFifoPeak(std::array<int32_t, 2>& peak, const std::array<float, 2>& parts, uint8_t volumes,
                         uint8_t sides)
{
    for (int f = 0; f < 2; f++)
    {
        const float part = parts[size_t(f)] * float(volumes >> (2 + f) & 1 ? 4 : 2) * 64.0f;
        const int32_t value = int32_t(std::lround(part));
        if (sides >> (4 * f) & 1)
        {
            peak[1] = std::max(peak[1], value);
        }

        if (sides >> (4 * f + 1) & 1)
        {
            peak[0] = std::max(peak[0], value);
        }
    }
}

// Raises each side of `peak` to that side of `part`.
inline void KeepPeak(std::array<int32_t, 2>& peak, const std::array<int32_t, 2>& part)
{
    peak[0] = std::max(peak[0], part[0]);
    peak[1] = std::max(peak[1], part[1]);
}

} // namespace agblib::agb
