// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/frontline_mod/soundfont.h"

#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/frontline_mod/driver.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::frontline_mod
{

using agblib::sequence_export::agb::Signed8Sample;

namespace
{

// The fraction bits of a step.
constexpr double kStepUnit = 65536;

} // namespace

double RootRate(const Rom& rom, const DriverInfo& info)
{
    return double(rom.U32(info.periods + 4 * kRootIndex)) * info.Rate() / kStepUnit;
}

bool Loops(const PlayedSample& sample)
{
    return sample.loop_end != 0 && sample.loop_start <= sample.loop_end;
}

int InstrumentSet::Program(const PlayedSample& sample, int number)
{
    return instruments_.Program(sample, number);
}

Sf2File InstrumentSet::Build(const Rom& rom) const
{
    const auto make_sample = [&](const PlayedSample& played, const char* name)
    {
        // A loop plays up to and including its end point.
        const uint32_t count = Loops(played) ? played.loop_end + 1 : played.length;
        return Signed8Sample(rom, played.points, count, name, played.rate, kRootKey, Loops(played), played.loop_start);
    };

    return instruments_.Build(make_sample);
}

} // namespace agblib::sequence_export::frontline_mod
