// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/frontline_xm/soundfont.h"

#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::frontline_xm
{

using agblib::frontline_xm::kPositionShift;
using agblib::frontline_xm::StepOf;
using agblib::sequence_export::agb::AddLoopGuard;
using agblib::sequence_export::agb::LengthenLoop;
namespace sf2 = seqlib::sf2;

namespace
{

// Returns the SoundFont sample of `played`: its points, signed 8-bit, and its loop.
sf2::Sample MakeSample(const Rom& rom, const PlayedSample& played, const char* name)
{
    sf2::Sample s;
    s.name = name;
    s.sample_rate = played.rate;
    s.original_key = uint8_t(kRootKey);
    const uint32_t count = played.loops ? played.loop_end : played.length;
    for (uint32_t i = 0; i < count; i++)
    {
        s.data.push_back(int16_t(rom.S8(played.points + i) * 256));
    }

    if (played.loops)
    {
        s.loop_start = played.loop_start;
        s.loop_end = count;
        LengthenLoop(s);
        AddLoopGuard(s);
    }

    if (s.data.empty())
    {
        s.data.push_back(0);
    }

    return s;
}

} // namespace

double RootRate(const Rom& rom, const DriverInfo& info, int8_t finetune)
{
    const uint32_t period = uint32_t(int32_t(kRootPeriod) - (int32_t(finetune) >> 1));
    return double(StepOf(rom, info, period)) * info.Rate(rom) / double(1u << kPositionShift);
}

int InstrumentSet::Program(const PlayedSample& sample, int number)
{
    return instruments_.Program(sample, number);
}

Sf2File InstrumentSet::Build(const Rom& rom) const
{
    const auto make_sample = [&](const PlayedSample& played, const char* name)
    {
        return MakeSample(rom, played, name);
    };

    return instruments_.Build(make_sample);
}

} // namespace agblib::sequence_export::frontline_xm
