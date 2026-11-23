// SPDX-License-Identifier: MIT

// A small synthetic cartridge with two drivers for the tests: Nintendo R&D2's driver in its first bytes and MP2K after
// them. Some games keep a second driver for a part of the game, beside the driver of the rest. The cartridge joins an
// R&D2 test cartridge (rd2_cart::Cart) and an MP2K test cartridge built with an origin (mp2k_cart::Cart).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/testing/mp2k_cart.h"
#include "agblib/testing/rd2_cart.h"

namespace agblib::testing::two_drivers_cart
{

// The origin of the MP2K cartridge. An R&D2 test cartridge's bank reads up to 64 KiB past the bank's start, and its
// bank starts at 0x4000. The MP2K cartridge starts past those bytes.
inline constexpr uint32_t kMp2kOrigin = 0x20000;

// The bytes of a cartridge whose first bytes are those of `rd2` and whose bytes from kMp2kOrigin on are those of
// `mp2k`, an MP2K cartridge with that origin. Throws std::invalid_argument when `rd2` reaches kMp2kOrigin or `mp2k` has
// another origin.
inline std::vector<uint8_t> TwoDrivers(const rd2_cart::Cart& rd2, const mp2k_cart::Cart& mp2k)
{
    const agb::Rom first = rd2.ToRom();
    const agb::Rom second = mp2k.ToRom();
    if (first.Size() > kMp2kOrigin || mp2k.At(agb::kRomBase) != agb::kRomBase + kMp2kOrigin)
    {
        throw std::invalid_argument("the MP2K cartridge has to start past the R&D2 cartridge, at kMp2kOrigin");
    }

    std::vector<uint8_t> bytes(second.Ptr(agb::kRomBase), second.Ptr(agb::kRomBase) + second.Size());
    std::copy_n(first.Ptr(agb::kRomBase), first.Size(), bytes.begin());

    return bytes;
}

} // namespace agblib::testing::two_drivers_cart
