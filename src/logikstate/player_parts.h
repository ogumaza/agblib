// SPDX-License-Identifier: MIT

// What the models of the driver's two builds share: the vibrato's wave, the target of a slide to a note and the layout
// of a channel's record.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"

#include "agblib/logikstate/driver.h"
#include "agblib/logikstate/player.h"

namespace agblib::logikstate::parts
{

// The first half of the vibrato's wave, 64 steps a cycle: ProTracker's sine table. The second half is the first,
// negated. Both builds keep the whole cycle as a table of 64 halfwords.
constexpr std::array<int16_t, 32> kSine = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
                                           224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
                                           212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

// Returns the vibrato's wave at `position`, from 0 to 63.
inline int32_t VibratoWave(uint16_t position)
{
    return position < 32 ? kSine[position & 31] : -kSine[position & 31];
}

// Takes a slide to a note's note from `cell` as the slide's target, and the volume of the cell's sample, if it names
// one. The cell then contains neither: the slide starts no note.
inline void TakeSlideTarget(Channel& ch, Cell& cell, const Module& module)
{
    if (cell.pitch != kNoPitch)
    {
        ch.slide_target = int16_t(cell.pitch);
    }

    cell.pitch = kNoPitch;
    if (cell.sample != 0)
    {
        ch.volume = module.samples[cell.sample - 1u].volume;
    }

    cell.sample = 0;
}

// Returns the first kChannelRecordSize bytes of a channel's record, in the layout of the memory of the build with short
// records and the GBAMOD1 build.
inline std::vector<uint8_t> RecordBytes(const Channel& ch)
{
    std::vector<uint8_t> out;
    xsflib::AppendLe32(out, ch.points);
    xsflib::AppendLe32(out, ch.position);
    xsflib::AppendLe32(out, ch.step);
    xsflib::AppendLe16(out, ch.length);
    xsflib::AppendLe16(out, ch.loop_start);
    xsflib::AppendLe16(out, ch.loop_length);
    xsflib::AppendLe16(out, 0);
    xsflib::AppendLe32(out, uint32_t(ch.volume));
    xsflib::AppendLe16(out, ch.sample);
    xsflib::AppendLe16(out, 0);
    xsflib::AppendLe16(out, uint16_t(ch.fine_tune));
    xsflib::AppendLe16(out, ch.pitch);
    xsflib::AppendLe32(out, uint32_t(ch.effect));
    xsflib::AppendLe16(out, ch.volume_up);
    xsflib::AppendLe16(out, ch.volume_down);
    xsflib::AppendLe16(out, uint16_t(ch.arpeggio_1));
    xsflib::AppendLe16(out, uint16_t(ch.arpeggio_2));
    xsflib::AppendLe16(out, uint16_t(ch.vibrato));
    xsflib::AppendLe16(out, ch.vibrato_depth);
    xsflib::AppendLe16(out, ch.vibrato_speed);
    xsflib::AppendLe16(out, ch.vibrato_position);
    xsflib::AppendLe16(out, uint16_t(ch.slide));
    xsflib::AppendLe16(out, ch.slide_speed);
    xsflib::AppendLe16(out, uint16_t(ch.slide_step));
    xsflib::AppendLe16(out, uint16_t(ch.slide_target));

    return out;
}

} // namespace agblib::logikstate::parts
