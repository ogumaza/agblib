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
using agblib::sequence_export::agb::Signed8Sample;

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
        const uint32_t count = played.loops ? played.loop_end : played.length;
        return Signed8Sample(rom, played.points, count, name, played.rate, kRootKey, played.loops, played.loop_start);
    };

    return instruments_.Build(make_sample);
}

} // namespace agblib::sequence_export::frontline_xm
