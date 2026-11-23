// SPDX-License-Identifier: MIT

// What the tests of the module players share: the hash of the bytes that a player writes for FIFO A. The local check
// tools print hashes with the same function.

#pragma once

#include <cstdint>

namespace agblib::testing::module_tests
{

// The start of the 64-bit FNV-1a hash.
constexpr uint64_t kFnv1aStart = 0xCBF29CE484222325ull;

// Returns `hash` with the FNV-1a step of 64 bits applied to each of `bytes`, a range of 8-bit values.
template <typename Bytes>
uint64_t Fnv1a(const Bytes& bytes, uint64_t hash = kFnv1aStart)
{
    for (const auto byte : bytes)
    {
        hash = (hash ^ uint8_t(byte)) * 0x100000001B3ull;
    }

    return hash;
}

// Runs `frames` frames of `player`, and returns the hash of the bytes of its output.
template <typename Player>
uint64_t OutputHash(Player& player, int frames)
{
    uint64_t hash = kFnv1aStart;
    for (int frame = 0; frame < frames; frame++)
    {
        player.Frame();
        hash = Fnv1a(player.Output(), hash);
    }

    return hash;
}

} // namespace agblib::testing::module_tests
