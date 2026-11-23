// SPDX-License-Identifier: MIT

// The arithmetic that player.cpp and player_mix.cpp share.

#pragma once

#include <cstdint>

namespace agblib::webfoot
{

// The pitch, the step and the slides multiply in 32 bits as the driver's `muls` does.
inline uint32_t Mul(uint32_t a, uint32_t b)
{
    return a * b;
}

} // namespace agblib::webfoot
