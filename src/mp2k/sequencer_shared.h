// SPDX-License-Identifier: MIT

// What the files of the MP2K sequencer share: the PSG channel registers and the decoder of compressed samples.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/wave/sinc.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/sound_registers.h"
#include "agblib/agb/warnings.h"
#include "agblib/mp2k/commands.h"
#include "agblib/mp2k/driver.h"
#include "agblib/mp2k/sequencer.h"
#include "agblib/mp2k/song.h"

namespace agblib::mp2k
{

// The most points that a frame of the mixer contains: those of its highest rate.
constexpr int kMostFramePoints = 704;

// The registers of the PSG's channels, NRx0 to NRx4. Channels 2 and 4 have no NRx0. The driver takes the byte after the
// previous channel's NRx0 there, and doesn't write it.
struct PsgRegisters
{
    uint32_t nrx0, nrx1, nrx2, nrx3, nrx4;
};

constexpr PsgRegisters kPsgRegisters[4] = {
    {agb::kRegNr10, agb::kRegNr11, agb::kRegNr12, agb::kRegNr13, agb::kRegNr14},
    {agb::kRegNr10 + 1, agb::kRegNr21, agb::kRegNr22, agb::kRegNr23, agb::kRegNr24},
    {agb::kRegNr30, agb::kRegNr31, agb::kRegNr32, agb::kRegNr33, agb::kRegNr34},
    {agb::kRegNr30 + 1, agb::kRegNr41, agb::kRegNr42, agb::kRegNr43, agb::kRegNr44},
};

// The steps of a compressed sample's points, by the 4 bits that code each one.
constexpr int8_t kDeltas[16] = {0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1};

// A compressed sample's points come in blocks of 64 points in 33 bytes. A block starts with its first point. Then the
// low 4 bits of the next byte give the second point's step from the first. The two halves of each later byte give the
// next two steps, the high half first. The points wrap round at 8 bits.
constexpr uint32_t kBlockPoints = 64;

constexpr uint32_t kBlockSize = 33;

// The decoded block of a compressed sample that the mixer last read.
struct DecodedBlock
{
    uint32_t index = ~0u;
    std::array<int8_t, kBlockPoints> points = {};
};

// Returns point `index` of the compressed sample at `wave`. Decodes the point's block into `block` first, unless
// `block` contains it already.
inline int32_t CompressedPoint(const Rom& rom, uint32_t wave, uint32_t index, DecodedBlock& block)
{
    if (block.index != index / kBlockPoints)
    {
        block.index = index / kBlockPoints;
        const uint32_t data = wave + 16 + kBlockSize * block.index;
        uint8_t point = rom.U8(data);
        block.points[0] = int8_t(point);
        for (uint32_t i = 1; i < kBlockPoints; i++)
        {
            const uint8_t byte = rom.U8(data + i / 2 + 1);
            point = uint8_t(point + kDeltas[i % 2 ? byte & 0xF : byte >> 4]);
            block.points[i] = int8_t(point);
        }
    }

    return block.points[index % kBlockPoints];
}

// The player's priority whose notes take a priority of 255 in a later version of Camelot's sequencer.
constexpr int kCamelotTopPlayer = 120;

// A track's flags.
constexpr uint8_t kFlagVolSet = 0x01;
constexpr uint8_t kFlagVolChange = 0x03;
constexpr uint8_t kFlagPitSet = 0x04;
constexpr uint8_t kFlagPitChange = 0x0C;
constexpr uint8_t kFlagVolPitChange = kFlagVolChange | kFlagPitChange;
constexpr uint8_t kFlagStart = 0x40;
constexpr uint8_t kFlagExist = 0x80;

// The settings that a track sets, for the conversion's reports.
constexpr uint8_t kSetVolume = 0x01;
constexpr uint8_t kSetPan = 0x02;
constexpr uint8_t kSetPitch = 0x04;

} // namespace agblib::mp2k
