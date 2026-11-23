// SPDX-License-Identifier: MIT

// What the tests of Logik State's drivers share: the test module's samples, the cells of a row and the detection of a
// test cartridge.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/logikstate/driver.h"
#include "agblib/testing/logikstate_cart.h"

namespace agblib::sequence_export::logikstate
{

// A module whose three samples have 100, 400 and 3000 points, full volume and no loop, with one pattern of 64 empty
// rows.
inline agblib::testing::logikstate_cart::TestModule ThreeSampleModule()
{
    agblib::testing::logikstate_cart::TestModule module;
    module.patterns = {std::vector<agblib::testing::logikstate_cart::TestRow>(64)};
    module.samples[0] = {100, 0, 64, 0, 0};
    module.samples[1] = {400, 0, 64, 0, 0};
    module.samples[2] = {3000, 0, 64, 0, 0};

    return module;
}

// A cell with a note of `sample` at `pitch`, and one with an effect alone.
inline agblib::testing::logikstate_cart::TestCell NoteCell(uint16_t pitch, uint16_t sample, uint8_t effect = 0,
                                                           uint8_t operand = 0)
{
    return {pitch, sample, effect, operand};
}

inline agblib::testing::logikstate_cart::TestCell EffectCell(uint8_t effect, uint8_t operand)
{
    return {agblib::logikstate::kNoPitch, 0, effect, operand};
}

// Detects the driver in `rom`. The info stays empty where detection fails.
inline agblib::logikstate::DriverInfo Detect(const agblib::agb::Rom& rom)
{
    agblib::logikstate::DriverInfo info;
    std::string error;
    agblib::logikstate::DetectDriver(rom, info, error);

    return info;
}

} // namespace agblib::sequence_export::logikstate
