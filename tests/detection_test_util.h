// SPDX-License-Identifier: MIT

// What the tests of the drivers' detection share: the check of a cartridge in which a driver's detection finds no
// driver.

#pragma once

#include <string>

#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"

namespace agblib::testing::detection_tests
{

// Checks that detect(rom, info, error), a driver's DetectDriver(), finds no driver in `rom` and gives no error.
template <typename Info, typename Detect>
void CheckNoDriver(const agblib::agb::Rom& rom, Detect detect)
{
    Info info;
    std::string error;
    const bool found = detect(rom, info, error);

    SEQLIB_CHECK(!found);
    SEQLIB_CHECK(error.empty());
}

} // namespace agblib::testing::detection_tests
