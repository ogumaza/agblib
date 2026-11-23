// SPDX-License-Identifier: MIT

// What the tests of R&D2's driver and its conversion share: the detection of a test cartridge and a run of a sequence.

#pragma once

#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/rd2/driver.h"
#include "agblib/rd2/sequencer.h"

namespace agblib::testing::rd2_tests
{

// Returns the driver's tables in `rom`. The ROM must have them.
inline rd2::DriverInfo Detect(const agb::Rom& rom)
{
    rd2::DriverInfo info;
    std::string error;
    SEQLIB_CHECK(rd2::DetectDriver(rom, rd2::DriverOverrides(), info, error));

    return info;
}

// Runs version `version` of a sequence for `frames` frames, and returns each frame's events.
inline std::vector<std::vector<rd2::Event>> Run(const agb::Rom& rom, const rd2::DriverInfo& info, int sequence,
                                                int frames, int version = 0)
{
    rd2::Sequencer seq(rom, info, sequence, version);
    std::vector<std::vector<rd2::Event>> out;
    for (int f = 0; f < frames; f++)
    {
        seq.Step();
        out.push_back(seq.Events());
    }

    return out;
}

} // namespace agblib::testing::rd2_tests
