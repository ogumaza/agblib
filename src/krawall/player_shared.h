// SPDX-License-Identifier: MIT

// What the files of the Krawall player share: the effects of the volume column, the BIOS division, the step to the next
// order, and the fields that every build's record of a mixer channel keeps in the same place.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

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

// Moves `order` to the next order of `module`, past the markers between songs (+++). Returns true if it passed a
// marker.
inline bool NextOrder(const ModuleInfo& module, uint8_t& order)
{
    order++;
    bool marker = false;
    while (module.orders[order] == kOrderSkip && order < module.order_count)
    {
        order++;
        marker = true;
    }

    return marker;
}

// Stores the fields of mixer channel `m` that every build keeps in the same place of its record, from the volume to
// the sides (0x00 to 0x1F).
inline void StoreMixFields(const MixChannel& m, std::vector<uint8_t>& r)
{
    r[0x00] = m.volume;
    r[0x01] = uint8_t(m.pan);
    r[0x02] = m.status;
    r[0x03] = m.loop;
    xsflib::StoreLe32(&r[0x04], m.start);
    xsflib::StoreLe32(&r[0x08], m.pos);
    xsflib::StoreLe32(&r[0x0C], m.end);
    xsflib::StoreLe32(&r[0x10], m.loop_length);
    xsflib::StoreLe32(&r[0x14], uint32_t(m.inc));
    xsflib::StoreLe32(&r[0x18], m.handle);
    xsflib::StoreLe16(&r[0x1C], m.frac);
    r[0x1E] = m.left;
    r[0x1F] = m.right;
}

} // namespace agblib::krawall
