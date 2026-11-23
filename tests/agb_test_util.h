// SPDX-License-Identifier: MIT

// What the tests of the GBA's drivers share: the check that a call throws, the comparisons of levels, a ROM of a test
// image, and a test program's set for archive mode.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/testing/near.h"

#include "agblib/agb/rom.h"

namespace agblib::testing::agb_tests
{

using seqlib::testing::Throws;

// Whether a level of an enhanced mix is within 0.5 % of `expected`, and 1e-6 more.
inline bool NearLevel(float value, double expected)
{
    return xsflib::testing::Near(value, expected, 0.005 * std::fabs(expected) + 1e-6);
}

// Whether `value` is within `within` of `expected`. Prints both when it isn't.
inline bool NearPrinted(float value, double expected, double within = 0.002)
{
    const bool near = xsflib::testing::Near(value, expected, within);
    if (!near)
    {
        std::printf("%f, expected %f\n", double(value), expected);
    }

    return near;
}

// A ROM of the image `bytes`.
inline agblib::agb::Rom RomOf(std::vector<uint8_t> bytes)
{
    agblib::agb::Rom rom;
    rom.Assign(std::move(bytes));

    return rom;
}

// A test program's set for archive mode: its files by path, and the song that it plays.
struct RomSet
{
    std::vector<uint8_t>* File(const std::string& path)
    {
        const auto file = files.find(path);
        return file != files.end() ? &file->second : nullptr;
    }

    std::map<std::string, std::vector<uint8_t>> files;
    int song = 0;
};

} // namespace agblib::testing::agb_tests
