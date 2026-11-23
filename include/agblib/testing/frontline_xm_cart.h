// SPDX-License-Identifier: MIT

// Synthetic data for tests of Frontline Studios' XM-like player: the tables that the player and the mixer read (the
// steps, the places of the periods, the waves and the rate's entries), four instruments and a module of 4 channels
// whose cells use every effect that the player follows and every command of the volume column, with notes, slides to
// a note both ways, a delayed note, breaks, jumps and the wrap to the first order.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <utility>
#include <vector>

#include "agblib/frontline_xm/driver.h"

namespace agblib::testing::frontline_xm_cart
{

using agblib::frontline_xm::kCellEffect;
using agblib::frontline_xm::kCellNote;
using agblib::frontline_xm::kCellVolume;

// The data's layout from its base: the tables, then the instruments and the module. The instruments and the module move
// with the base.
constexpr uint32_t kSteps = 0x0000;
constexpr uint32_t kStepCount = 768;
constexpr uint32_t kPeriods = 0x0C00;
constexpr uint32_t kPeriodCount = 7680;
constexpr uint32_t kWaves = 0x4800; // the sine, the square and the saw, 256 signed bytes each
constexpr uint32_t kWaveSize = 0x300;
constexpr uint32_t kRates = 0x4B00;
constexpr uint32_t kFrameSamples = 0x4B10;
constexpr uint32_t kStepRates = 0x4B20;
constexpr uint32_t kSilence = 0x4B40;
constexpr uint32_t kMusic = 0x5000; // the instruments and the module
constexpr uint32_t kSpare = 0x7000; // free for a test's module
constexpr uint32_t kSize = 0x8000;

// A module's header.
constexpr uint32_t kHeaderSize = 0x14;

// The rate's index that the games use, its rate and its samples a frame. The step's factor of 2^18 makes a step the
// table's value.
constexpr uint8_t kRateIndex = 6;
constexpr uint16_t kRate = 20068;
constexpr uint16_t kFrameSampleCount = 336;
constexpr uint32_t kStepRate = 1u << 18;

// The module's set-up: its speed and BPM, and the master volumes of the two songs.
constexpr uint8_t kSpeed = 3;
constexpr uint8_t kBpm = 125;
constexpr uint8_t kFirstVolume = 200;
constexpr uint8_t kSecondVolume = 120;

// A cell: a note and an instrument, a byte of the volume column and an effect. -1 leaves a part out.
struct Cell
{
    int note = -1;
    int instrument = 0;
    int volume = -1;
    int effect = -1;
    int parameter = 0;
};

using Row = std::vector<Cell>; // a cell for each channel

// The instruments.
constexpr int kTriangle = 0; // 64 points, looped from 16 to 64
constexpr int kNoise = 1;    // 600 points without a loop
constexpr int kEmpty = 2;    // no points
constexpr int kSaw = 3;      // 48 points and a half, looped from 10 points and a fraction

class Data
{
public:
    // Builds the data for the address `base`.
    explicit Data(uint32_t base) : base_(base), bytes_(kSize, 0)
    {
        // The steps rise through each block of 768 periods, and each block of periods shifts its steps one more bit
        // to the right. Integers keep the data the same on every platform.
        for (uint32_t k = 0; k < kStepCount; k++)
        {
            Put32(kSteps + 4 * k, 0x20000 + 0x100 * k);
        }

        for (uint32_t p = 0; p < kPeriodCount; p++)
        {
            Put16(kPeriods + 2 * p, uint16_t(((kStepCount - 1 - p % kStepCount) << 4) | (p / kStepCount)));
        }

        // The waves: a triangle for the sine, a square, and a falling saw.
        for (int i = 0; i < 256; i++)
        {
            const int triangle = i < 64 ? 2 * i : i < 192 ? 255 - 2 * i : 2 * i - 512;
            bytes_[kWaves + uint32_t(i)] = uint8_t(int8_t(std::max(-128, std::min(127, triangle))));
            bytes_[kWaves + 256 + uint32_t(i)] = uint8_t(int8_t(i < 128 ? 127 : -128));
            bytes_[kWaves + 512 + uint32_t(i)] = uint8_t(int8_t(127 - i));
        }

        Put16(kRates + 2 * kRateIndex, kRate);
        Put16(kFrameSamples + 2 * kRateIndex, kFrameSampleCount);
        Put32(kStepRates + 4 * kRateIndex, kStepRate);

        PutMusic();
    }

    uint32_t Base() const
    {
        return base_;
    }

    const std::vector<uint8_t>& Bytes() const
    {
        return bytes_;
    }

    // The module's header, its order list and the songs' second list.
    uint32_t Module() const
    {
        return base_ + module_;
    }

    uint32_t Orders() const
    {
        const uint32_t at = module_ + agblib::frontline_xm::kHeaderOrders;
        return uint32_t(bytes_[at]) | (uint32_t(bytes_[at + 1]) << 8) | (uint32_t(bytes_[at + 2]) << 16) |
               (uint32_t(bytes_[at + 3]) << 24);
    }

    uint8_t LastOrder() const
    {
        return last_order_;
    }

    uint32_t SecondList() const
    {
        return base_ + second_list_;
    }

    uint8_t SecondLast() const
    {
        return second_last_;
    }

private:
    void Put8(uint32_t at, uint8_t v)
    {
        bytes_[at] = v;
    }

    void Put16(uint32_t at, uint16_t v)
    {
        Put8(at, uint8_t(v));
        Put8(at + 1, uint8_t(v >> 8));
    }

    void Put32(uint32_t at, uint32_t v)
    {
        Put16(at, uint16_t(v));
        Put16(at + 2, uint16_t(v >> 16));
    }

    // Writes an instrument's record and its points at `at`. Returns the end, aligned to a word as in the games: the
    // player reads the record's words with word loads.
    uint32_t PutInstrument(uint32_t at, uint32_t length, uint32_t loop_start, uint32_t loop_end, uint8_t volume,
                           int8_t finetune, uint8_t loop_type, uint32_t points,
                           const std::function<int(uint32_t)>& point)
    {
        Put32(at + agblib::frontline_xm::kInstrumentLength, length);
        Put32(at + agblib::frontline_xm::kInstrumentLoopStart, loop_start);
        Put32(at + agblib::frontline_xm::kInstrumentLoopEnd, loop_end);
        Put8(at + agblib::frontline_xm::kInstrumentVolume, volume);
        Put8(at + agblib::frontline_xm::kInstrumentFinetune, uint8_t(finetune));
        Put8(at + agblib::frontline_xm::kInstrumentLoopType, loop_type);
        for (uint32_t i = 0; i < points; i++)
        {
            Put8(at + agblib::frontline_xm::kInstrumentPoints + i, uint8_t(int8_t(point(i))));
        }

        return (at + agblib::frontline_xm::kInstrumentPoints + points + 3) & ~3u;
    }

    uint32_t PutCell(uint32_t at, const Cell& cell)
    {
        const uint8_t flags = uint8_t((cell.note >= 0 ? kCellNote : 0) | (cell.volume >= 0 ? kCellVolume : 0) |
                                      (cell.effect >= 0 ? kCellEffect : 0));
        Put8(at++, flags);
        if (cell.note >= 0)
        {
            Put8(at++, uint8_t(cell.note));
            Put8(at++, uint8_t(cell.instrument));
        }

        if (cell.volume >= 0)
        {
            Put8(at++, uint8_t(cell.volume));
        }

        if (cell.effect >= 0)
        {
            Put8(at++, uint8_t(cell.effect));
            Put8(at++, uint8_t(cell.parameter));
        }

        return at;
    }

    // The instruments, and the module with its two lists.
    void PutMusic()
    {
        constexpr uint32_t kPoint = 1u << agblib::frontline_xm::kPositionShift;
        uint32_t at = kMusic;
        std::vector<uint32_t> instruments;
        instruments.push_back(at);
        at = PutInstrument(at, 64 * kPoint, 16 * kPoint, 64 * kPoint, 64, 0, 1, 64,
                           [](uint32_t i) { return int(i < 32 ? -120 + 8 * i : 136 - 8 * (i - 32)); });
        instruments.push_back(at);
        uint32_t seed = 12345;
        at = PutInstrument(at, 600 * kPoint, 0, 0, 40, -20, 0, 600,
                           [&seed](uint32_t)
                           {
                               seed = seed * 1103515245u + 12345u;
                               return int((seed >> 16) & 0xFF) - 128;
                           });
        instruments.push_back(at);
        at = PutInstrument(at, 0, 0, 0, 30, 5, 0, 0, [](uint32_t) { return 0; });
        instruments.push_back(at);
        const uint32_t saw_end = 48 * kPoint + 0x1000;
        at = PutInstrument(at, saw_end, 10 * kPoint + 0x234, saw_end, 50, 12, 1, 49,
                           [](uint32_t i) { return int(i % 12) * 20 - 110; });

        instrument_table_ = at;
        for (size_t i = 0; i < instruments.size(); i++)
        {
            Put32(instrument_table_ + 4 * uint32_t(i), base_ + instruments[i]);
        }

        at = instrument_table_ + 4 * uint32_t(instruments.size());

        // The module's list plays every pattern. The second list plays pattern 3, then pattern 1, whose jump passes
        // the list's last order.
        const std::vector<uint8_t> orders = {0, 1, 2, 3};
        at = PutModule(at, kSpeed, {PatternZero(), PatternOne(), PatternTwo(), PatternThree()}, orders);
        module_ = at - kHeaderSize;
        last_order_ = uint8_t(orders.size() - 1);
        second_list_ = at;
        for (const uint8_t order : std::initializer_list<uint8_t>{3, 1})
        {
            Put8(at++, order);
        }

        second_last_ = 1;
    }

public:
    // Writes a module of 4 channels from `at` on, with the data's instruments: its patterns, its order list, then its
    // header, word-aligned. Returns the end of the header.
    uint32_t PutModule(uint32_t at, uint8_t speed, const std::vector<std::vector<Row>>& patterns,
                       const std::vector<uint8_t>& orders)
    {
        const uint32_t pattern_table = at;
        at += 4 * uint32_t(patterns.size());
        for (size_t p = 0; p < patterns.size(); p++)
        {
            Put32(pattern_table + 4 * uint32_t(p), base_ + at);
            Put16(at, uint16_t(patterns[p].size()));
            at += agblib::frontline_xm::kPatternCells;
            for (const Row& row : patterns[p])
            {
                for (const Cell& cell : row)
                {
                    at = PutCell(at, cell);
                }
            }
        }

        const uint32_t order_list = at;
        for (const uint8_t order : orders)
        {
            Put8(at++, order);
        }

        const uint32_t header = (at + 3) & ~3u;
        Put8(header + agblib::frontline_xm::kHeaderChannels, 4);
        Put8(header + agblib::frontline_xm::kHeaderPatternCount, uint8_t(patterns.size()));
        Put8(header + agblib::frontline_xm::kHeaderMaster, 0x40);
        Put8(header + agblib::frontline_xm::kHeaderNoteHandler, 0);
        Put8(header + agblib::frontline_xm::kHeaderLastOrder, uint8_t(orders.size() - 1));
        Put8(header + agblib::frontline_xm::kHeaderSpeed, speed);
        Put8(header + agblib::frontline_xm::kHeaderBpm, kBpm);
        Put32(header + agblib::frontline_xm::kHeaderOrders, base_ + order_list);
        Put32(header + agblib::frontline_xm::kHeaderPatterns, base_ + pattern_table);
        Put32(header + agblib::frontline_xm::kHeaderInstruments, base_ + instrument_table_);
        return header + kHeaderSize;
    }

private:
    // Pattern 0: notes on every channel, the slides and their memories, the volume column's commands, vibratos from
    // the effect and the column, an arpeggio, a tremolo, the fine slides, an offset, a cut, a delayed note with a
    // volume, effect 8 with and without a volume, a BPM, and a break to row 2.
    static std::vector<Row> PatternZero()
    {
        const Cell e;
        return {
            {{48, kTriangle, 0x50, -1, 0}, {50, kNoise, -1, 5, 40}, {40, kSaw, -1, -1, 0}, {55, kTriangle, -1, 9, 4}},
            {{-1, 0, -1, 2, 3}, {-1, 0, 0x65, -1, 0}, {-1, 0, -1, 7, 0x46}, {-1, 0, -1, 6, 0x30}},
            {{-1, 0, -1, 2, 0}, {-1, 0, 0x74, -1, 0}, {-1, 0, -1, 7, 0}, {-1, 0, -1, 6, 0x05}},
            {{-1, 0, -1, 3, 2}, {-1, 0, 0x83, -1, 0}, {-1, 0, 0xA5, -1, 0}, {-1, 0, -1, 6, 0x25}},
            {{60, kTriangle, -1, 4, 6}, {-1, 0, 0x94, -1, 0}, {-1, 0, 0xB6, -1, 0}, {-1, 0, -1, 10, 0x47}},
            {{-1, 0, -1, 4, 0}, {45, kNoise, 0xF3, -1, 0}, e, {-1, 0, -1, 13, 0x38}},
            {{30, kTriangle, 0xF8, 4, 2}, {-1, 0, 0xC5, -1, 0}, {-1, 0, -1, 20, 0}, {-1, 0, -1, 13, 0}},
            {{-1, 0, -1, 17, 3}, {-1, 0, -1, 18, 0x50}, {-1, 0, -1, 7, 0x23}, {-1, 0, -1, 23, 1}},
            {{50, kSaw, -1, 15, 1}, {50, kNoise, -1, 15, 1}, {-1, 0, -1, 25, 10}, {-1, 0, -1, 26, 7}},
            {{-1, 0, -1, 27, 1}, {52, kNoise, 0x30, 28, 2}, {-1, 0, -1, 24, 2}, {-1, 0, -1, 29, 3}},
            {{47, kEmpty, -1, -1, 0}, e, {-1, 0, 0x40, 8, 0}, {-1, 0, -1, 8, 0}},
            {{49, kTriangle, -1, -1, 0}, {-1, 0, -1, 1, 150}, {-1, 0, -1, 20, 1}, {-1, 0, -1, 5, 30}},
            {{-1, 0, 0x55, -1, 0}, {70, kSaw, -1, 4, 0}, {-1, 0, 0x9F, -1, 0}, {-1, 0, 0x8F, -1, 0}},
            {{-1, 0, -1, 16, 2}, e, e, e},
        };
    }

    // Pattern 1: new notes, the tremolo's wave, a jump to order 2 at the next tick, and rows that the jump skips.
    static std::vector<Row> PatternOne()
    {
        const Cell e;
        return {
            {{40, kTriangle, -1, -1, 0}, {44, kNoise, -1, -1, 0}, {50, kSaw, -1, 7, 0x11}, {36, kTriangle, -1, 23, 0}},
            {{-1, 0, 0x12, -1, 0}, {-1, 0, -1, 3, 1}, e, {-1, 0, -1, 13, 0x5F}},
            {{-1, 0, -1, 14, 2}, e, e, e},
            {{62, kSaw, -1, -1, 0}, e, e, e},
            {e, e, {64, kNoise, -1, -1, 0}, e},
        };
    }

    // Pattern 2: a break to a row past the next pattern's end. The player then plays the next pattern from its first
    // row.
    static std::vector<Row> PatternTwo()
    {
        const Cell e;
        return {
            {{43, kSaw, -1, 6, 0x20}, {47, kTriangle, -1, -1, 0}, e, {52, kNoise, 0x18, -1, 0}},
            {e, {-1, 0, -1, 16, 99}, e, e},
        };
    }

    // Pattern 3: speed 1, at which no tick runs the channels' routines, then speed 3, and a break to row 1 in its last
    // row.
    static std::vector<Row> PatternThree()
    {
        const Cell e;
        return {
            {{55, kTriangle, -1, 9, 1}, {57, kSaw, -1, 2, 8}, {59, kNoise, -1, -1, 0}, {60, kTriangle, 0x70, -1, 0}},
            {e, e, e, e},
            {{-1, 0, -1, 9, 3}, e, e, e},
            {e, e, e, e},
            {{38, kNoise, -1, -1, 0}, e, e, e},
            {e, {-1, 0, -1, 16, 1}, e, {-1, 0, -1, 7, 0x84}},
        };
    }

    uint32_t base_;
    std::vector<uint8_t> bytes_;
    uint32_t instrument_table_ = 0;
    uint32_t module_ = 0;
    uint8_t last_order_ = 0;
    uint32_t second_list_ = 0;
    uint8_t second_last_ = 0;
};

} // namespace agblib::testing::frontline_xm_cart
