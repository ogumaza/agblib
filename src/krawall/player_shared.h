// SPDX-License-Identifier: MIT

// What the files of the Krawall player share: the effects of the volume column and the BIOS division.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "xsflib/bytes.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/warnings.h"
#include "agblib/krawall/build.h"
#include "agblib/krawall/driver.h"
#include "agblib/krawall/effects.h"
#include "agblib/krawall/player.h"

namespace agblib::krawall
{

// The volume column's effects, by the column's high digit from 6.
enum ColumnEffect : int
{
    kColumnVolumeSlideDown = 0,
    kColumnVolumeSlideUp = 1,
    kColumnFineVolumeDown = 2,
    kColumnFineVolumeUp = 3,
    kColumnVibratoSpeed = 4,
    kColumnVibrato = 5,
    kColumnPan = 6,
    kColumnPanSlideLeft = 7,
    kColumnPanSlideRight = 8,
    kColumnPortaNote = 9,
};

// Returns the quotient and remainder from the BIOS's signed Div operation. For a zero divisor, the player skips the
// call and uses `n` and 0.
inline std::pair<int32_t, int32_t> Div(int32_t n, int32_t d)
{
    if (d == 0)
    {
        return {n, 0};
    }
    if (n == INT32_MIN && d == -1)
    {
        return {n, 0};
    }

    return {n / d, n % d};
}

} // namespace agblib::krawall
