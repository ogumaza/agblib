// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/engine_software/soundfont.h"

#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::engine_software
{

using agblib::engine_software::kFractionBits;
using agblib::engine_software::kInstrumentFineTune;
using agblib::engine_software::kInstrumentRelativeNote;
using agblib::engine_software::kMiddleNote;
using agblib::engine_software::kNoteSteps;
using agblib::engine_software::PitchStep;
using agblib::sequence_export::agb::Signed8Sample;

double RootRate(const Rom& rom, const DriverInfo& info, uint32_t record)
{
    const int32_t index =
        (rom.S8(record + kInstrumentRelativeNote) + kMiddleNote) * kNoteSteps + rom.S8(record + kInstrumentFineTune);
    return double(PitchStep(rom, info, index)) * info.OutputRate() / double(1u << kFractionBits);
}

int InstrumentSet::Program(const PlayedSample& sample, int number)
{
    return instruments_.Program(sample, number);
}

Sf2File InstrumentSet::Build(const Rom& rom) const
{
    const auto make_sample = [&](const PlayedSample& played, const char* name)
    {
        return Signed8Sample(rom, played.points, played.length, name, played.rate, kRootKey, played.loops,
                             played.loop_start);
    };

    return instruments_.Build(make_sample);
}

} // namespace agblib::sequence_export::engine_software
