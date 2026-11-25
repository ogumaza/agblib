// SPDX-License-Identifier: MIT

// The match of a pattern of masked code units, ARM words or Thumb halfwords, at an address of a cartridge
// (MatchesArm() and MatchesThumb()).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "agblib/agb/rom.h"

namespace agblib::agb
{

// Returns true if each unit of code from `at` in `rom`, masked by its entry of `pattern`, equals the entry's value.
// `Pattern` has the fields `value` and `mask`, as wide as a unit: 2 bytes for Thumb, 4 for ARM.
template <typename Pattern>
bool MatchesCode(const Rom& rom, uint32_t at, std::span<const Pattern> pattern)
{
    constexpr uint32_t kUnit = sizeof(decltype(Pattern::value));
    static_assert(kUnit == 2 || kUnit == 4);
    if (!rom.Contains(at, kUnit * uint32_t(pattern.size())))
    {
        return false;
    }

    for (size_t i = 0; i < pattern.size(); i++)
    {
        const uint32_t place = at + kUnit * uint32_t(i);
        const uint32_t unit = kUnit == 2 ? rom.U16(place) : rom.U32(place);
        if ((unit & pattern[i].mask) != pattern[i].value)
        {
            return false;
        }
    }

    return true;
}

} // namespace agblib::agb
