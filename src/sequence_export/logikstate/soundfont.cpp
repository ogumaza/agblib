// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/logikstate/soundfont.h"

#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/logikstate/driver.h"
#include "agblib/logikstate/player.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::logikstate
{

using agblib::logikstate::kModulePeriods;
using agblib::logikstate::kPeriodClock;
using agblib::logikstate::kStoppedLength;
using agblib::sequence_export::agb::AddLoopGuard;
using agblib::sequence_export::agb::LengthenLoop;
namespace sf2 = seqlib::sf2;

namespace
{

// The rate of kRootPitch from ProTracker's period for it, for a module whose table has no period there.
constexpr uint32_t kUsualRate = 8287;

// Returns the SoundFont sample of `played`: its points, signed 8-bit, and its loop.
sf2::Sample MakeSample(const Rom& rom, const PlayedSample& played, const char* name)
{
    sf2::Sample s;
    s.name = name;
    s.sample_rate = played.rate;
    s.original_key = uint8_t(kRootKey);
    const auto add = [&](uint32_t from, uint32_t count)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            s.data.push_back(int16_t(rom.S8(played.points + from + i) * 256));
        }
    };

    add(0, played.length);

    if (played.loops)
    {
        s.loop_start = LoopOffset(played);
        if (s.loop_start == played.length)
        {
            add(played.loop_start, played.loop_length);
        }

        s.loop_end = uint32_t(s.data.size());
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

bool Loops(uint32_t loop_length)
{
    return loop_length > kStoppedLength;
}

uint32_t LoopOffset(const PlayedSample& sample)
{
    const bool ends_together = sample.loop_start + sample.loop_length == sample.length;
    return ends_together ? sample.loop_start : sample.length;
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

uint32_t SampleRate(const Rom& rom, const Module& module, uint16_t fine_tune)
{
    if (module.format == agblib::logikstate::Format::kGbaMod1)
    {
        const uint64_t step = rom.U32(module.steps + (kMod1RootPitch + fine_tune) * 4);
        return step ? uint32_t((step * module.rate + 2048) / 4096) : kUsualRate;
    }

    const uint32_t period = rom.U16(module.address + kModulePeriods + (kRootPitch + fine_tune) * 2);
    return period ? uint32_t(kPeriodClock) / (period * 2) : kUsualRate;
}

} // namespace agblib::sequence_export::logikstate
