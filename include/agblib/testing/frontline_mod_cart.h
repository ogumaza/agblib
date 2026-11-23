// SPDX-License-Identifier: MIT

// Synthetic data for tests of Frontline Studios' MOD player: a table of periods, a vibrato table, three samples and a
// module of 4 channels whose cells use every effect of the player's table but the last, with notes, slides to a
// note both ways, a pattern of 0 rows, jumps and the wrap to the first order.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "agblib/frontline_mod/driver.h"

namespace agblib::testing::frontline_mod_cart
{

using agblib::frontline_mod::kCellEffect;
using agblib::frontline_mod::kCellNote;
using agblib::frontline_mod::kCellSlideTarget;
using agblib::frontline_mod::kCellVolume;

// The data's layout from its base: the table of periods, the vibrato table, the samples, the instrument table and the
// instruments, and the module with its orders and patterns.
constexpr uint32_t kPeriods = 0x0000;
constexpr uint32_t kPeriodCount = 1024;
constexpr uint32_t kVibrato = 0x1000;
constexpr uint32_t kSamples = 0x1100;
constexpr uint32_t kInstruments = 0x2000;
constexpr uint32_t kModule = 0x2100;
constexpr uint32_t kSpare = 0x3000; // free for a test's module
constexpr uint32_t kSize = 0x4000;

// The step of period index 0, and how much each index adds. Index 896 steps a point a sample.
constexpr uint32_t kFirstStep = 0x2000;
constexpr uint32_t kStepPerIndex = 64;

// A second table of periods, after the data, for the tests of conversions. It gives the step of three indices and 0
// for the rest: the index that plays 8,363 points a second, the index an octave above it, and the index a quarter of a
// semitone below it. The games' table gives the same steps to the first two.
constexpr uint32_t kGameTable = kSize;
constexpr uint32_t kRootIndex = 4608;
constexpr uint32_t kRootStep = 30185;
constexpr uint32_t kOctaveIndex = kRootIndex - 768;
constexpr uint32_t kOctaveStep = 2 * kRootStep;
constexpr uint32_t kFlatIndex = kRootIndex + 16;
constexpr uint32_t kFlatStep = 29752; // 30185 x 2^(-1/48)

// Returns `bytes`, the data from its base, with the second table of periods after it.
inline std::vector<uint8_t> WithGameTable(std::vector<uint8_t> bytes)
{
    bytes.resize(kGameTable + 4 * (kFlatIndex + 1), 0);
    const auto put = [&](uint32_t index, uint32_t step)
    {
        for (uint32_t k = 0; k < 4; k++)
        {
            bytes[kGameTable + 4 * index + k] = uint8_t(step >> (8 * k));
        }
    };
    put(kRootIndex, kRootStep);
    put(kOctaveIndex, kOctaveStep);
    put(kFlatIndex, kFlatStep);

    return bytes;
}

// The orders of the module, and the patterns that they name: pattern 0 (16 rows), pattern 1 (0 rows: the player plays
// one) and pattern 2 (8 rows).
constexpr uint32_t kOrderCount = 3;
constexpr uint32_t kSpeed = 3;

// A cell: a note (a period index and an instrument, or the target of a slide), a volume and an effect.
struct Cell
{
    int index = -1;      // the period index, or -1 for no note
    int instrument = 0;  // 0 keeps the channel's last instrument
    bool target = false; // the note is the target of a slide (flags 0x11)
    int volume = -1;
    int effect = 0;
    int parameter = 0;
};

using Row = std::vector<Cell>; // a cell for each channel

// A pattern: its rows, and the count of rows that its record gives. A count of 0 plays the first row.
struct Pattern
{
    // A pattern converts from its rows, with their count.
    Pattern(std::vector<Row> pattern_rows) : rows(std::move(pattern_rows)), count(uint32_t(rows.size()))
    {
    }

    Pattern(std::vector<Row> pattern_rows, uint32_t row_count) : rows(std::move(pattern_rows)), count(row_count)
    {
    }

    std::vector<Row> rows;
    uint32_t count;
};

// The instruments of the data.
constexpr int kTriangle = 1; // 64 points, looped from 0 to 64
constexpr int kNoise = 2;    // 400 points without a loop
constexpr int kSaw = 3;      // 300 points, looped from 100 to 300

class Data
{
public:
    // Builds the data for the address `base`.
    explicit Data(uint32_t base) : base_(base), bytes_(kSize, 0)
    {
        // Periods: a straight line through the unit step. Integers keep the data the same on every platform.
        for (uint32_t i = 0; i < kPeriodCount; i++)
        {
            Put32(kPeriods + 4 * i, kFirstStep + kStepPerIndex * i);
        }

        // A triangle of 64 steps: 127 at a quarter, -128 at three quarters.
        for (int i = 0; i < 64; i++)
        {
            const int value = i <= 16 ? i * 127 / 16 : i <= 48 ? 127 - (i - 16) * 255 / 32 : -128 + (i - 48) * 128 / 16;
            bytes_[kVibrato + uint32_t(i)] = uint8_t(int8_t(value));
        }

        // Samples: a looped triangle of 64 points, 400 points of noise without a loop, and a saw of 300 points with a
        // loop from 100 to 300.
        const uint32_t triangle = kSamples;
        const uint32_t noise = triangle + 8 + 64;
        const uint32_t saw = noise + 8 + 400;
        PutSample(triangle, 64, [](uint32_t i) { return int(i < 32 ? -120 + 8 * i : 136 - 8 * (i - 32)); });
        uint32_t seed = 12345;
        PutSample(noise, 400,
                  [&seed](uint32_t)
                  {
                      seed = seed * 1103515245u + 12345u;
                      return int((seed >> 16) & 0xFF) - 128;
                  });
        PutSample(saw, 300, [](uint32_t i) { return int(i % 50) * 5 - 125; });

        // Instruments: 1 the triangle, 2 the noise, 3 the saw. Entry 0 is 0, as in the games.
        const uint32_t records = kInstruments + 16;
        PutInstrument(kTriangle, records, 0, 64, 64, triangle);
        PutInstrument(kNoise, records + 16, 0, 0, 40, noise);
        PutInstrument(kSaw, records + 32, 100, 300, 50, saw);

        PutModule(kModule, kSpeed, {PatternZero(), {PatternOne(), 0}, PatternTwo()}, {0, 1, 2});
    }

    // Writes a module of 4 channels at `at` from the base, with the data's instruments: its header, then its orders,
    // then the patterns' records and cells. The orders name `patterns` by their places.
    void PutModule(uint32_t at, uint32_t speed, const std::vector<Pattern>& patterns,
                   const std::vector<uint32_t>& orders)
    {
        const uint32_t order_table = at + 0x18;
        const uint32_t records = order_table + 4 * uint32_t(orders.size());
        Put32(at + 0x00, 4);
        Put32(at + 0x04, uint32_t(orders.size()));
        Put32(at + 0x08, uint32_t(patterns.size()));
        Put32(at + 0x0C, speed);
        Put32(at + 0x10, base_ + kInstruments);
        Put32(at + 0x14, base_ + order_table);
        for (size_t k = 0; k < orders.size(); k++)
        {
            Put32(order_table + 4 * uint32_t(k), base_ + records + 8 * orders[k]);
        }

        uint32_t cells = records + 8 * uint32_t(patterns.size());
        for (uint32_t p = 0; p < uint32_t(patterns.size()); p++)
        {
            Put32(records + 8 * p, patterns[p].count);
            Put32(records + 8 * p + 4, base_ + cells);
            for (const Row& row : patterns[p].rows)
            {
                for (const Cell& cell : row)
                {
                    cells = PutCell(cells, cell);
                }
            }
        }
    }

    uint32_t Base() const
    {
        return base_;
    }

    const std::vector<uint8_t>& Bytes() const
    {
        return bytes_;
    }

private:
    // Pattern 0: notes, slides, the volume's effects, vibratos, the effects that do nothing, the speeds and a jump
    // whose row plays on.
    static std::vector<Row> PatternZero()
    {
        const Cell e;
        return {
            {{512, 1, false, -1, 9, 3}, {520, 2, false, 48}, {500, 3, false, -1, 15, 0}, {530, 1, false, -1, 7, 0x48}},
            {{-1, 0, false, -1, 2, 2}, {-1, 0, false, -1, 6, 0x03}, e, {-1, 0, false, -1, 8, 0}},
            {{-1, 0, false, -1, 3, 3}, {-1, 0, false, -1, 6, 0x20}, {512, 0, false, -1, 15, 1}, e},
            {{560, 0, true, -1, 4, 2}, {-1, 0, false, -1, 6, 0x15}, e, {-1, 0, false, -1, 12, 0x12}},
            {{-1, 0, false, -1, 4, 0},
             {-1, 0, false, -1, 5, 80},
             {-1, 0, false, -1, 1, 9},
             {-1, 0, false, -1, 12, 0x31}},
            {{480, 0, true, -1, 4, 4}, {-1, 0, false, -1, 10, 5}, {-1, 0, false, -1, 11, 6}, {-1, 0, false, -1, 13, 7}},
            {{-1, 0, false, -1, 16, 1},
             {-1, 0, false, -1, 20, 2},
             {-1, 0, false, -1, 25, 3},
             {-1, 0, false, -1, 30, 4}},
            {{512, 2, false, 20, 7, 0x3F}, e, {-1, 0, false, -1, 9, 0}, {-1, 0, false, 64}},
            {e, {700, 2, false, -1, 0, 0}, {-1, 0, false, -1, 9, 1}, e},
            {{-1, 0, false, -1, 8, 0x21}, e, {-1, 0, false, -1, 9, 4}, {-1, 0, false, 64, 6, 0x0F}},
            {{540, 3, false, 60}, {-1, 0, false, -1, 6, 0xF0}, {496, 1}, {-1, 0, false, -1, 6, 0x0F}},
            {{-1, 0, false, -1, 6, 0x0F}, {800, 2, false, -1, 15, 0}, e, {-1, 0, false, -1, 6, 0x0F}},
            {e, e, {-1, 0, false, -1, 3, 0}, {528, 1, false, 30}},
            {{-1, 0, false, -1, 1, 0}, {-1, 0, false, -1, 2, 0}, e, {-1, 0, false, -1, 7, 0}},
            {{520, 1}, e, {-1, 0, false, -1, 6, 0x24}, e},
            {{-1, 0, false, -1, 7, 0x84}, {-1, 0, false, -1, 14, 1}, {506, 3, false, 64}, {-1, 0, false, -1, 5, 10}},
        };
    }

    // Pattern 1: no rows. The player reads one.
    static std::vector<Row> PatternOne()
    {
        const Cell e;
        return {{{512, 1}, e, {-1, 0, false, -1, 4, 3}, e}};
    }

    // Pattern 2: the volume slide with a vibrato, notes without an instrument, and the wrap to the first order.
    static std::vector<Row> PatternTwo()
    {
        const Cell e;
        return {
            {{520, 3, false, -1, 12, 0x01}, {530, 1}, {490, 2, false, 50}, {515, 3, false, -1, 7, 0x26}},
            {{-1, 0, false, -1, 12, 0x10}, {-1, 0, false, -1, 3, 1}, e, {-1, 0, false, -1, 12, 0x02}},
            {e, e, e, e},
            {{600, 0, true, -1, 4, 8}, {-1, 0, false, -1, 9, 2}, {512, 3, false, -1, 15, 0}, e},
            {{-1, 0, false, -1, 4, 0}, e, e, {-1, 0, false, -1, 8, 0}},
            {{-1, 0, false, 32}, {-1, 0, false, -1, 2, 1}, e, e},
            {{512, 2, false, -1, 6, 0x0F}, e, {500, 0}, e},
            {{-1, 0, false, -1, 6, 0x0F}, {-1, 0, false, -1, 9, 3}, e, {-1, 0, false, -1, 6, 0x0F}},
        };
    }

    void Put32(uint32_t at, uint32_t value)
    {
        for (int k = 0; k < 4; k++)
        {
            bytes_[at + k] = uint8_t(value >> (8 * k));
        }
    }

    template <typename Point>
    void PutSample(uint32_t at, uint32_t length, Point point)
    {
        Put32(at, length);
        Put32(at + 4, base_ + at + 8);
        for (uint32_t i = 0; i < length; i++)
        {
            bytes_[at + 8 + i] = uint8_t(int8_t(point(i)));
        }
    }

    void PutInstrument(uint32_t number, uint32_t at, uint32_t loop_start, uint32_t loop_end, uint32_t volume,
                       uint32_t sample)
    {
        Put32(kInstruments + 4 * number, base_ + at);
        Put32(at + 0x0, loop_start);
        Put32(at + 0x4, loop_end);
        Put32(at + 0x8, volume);
        Put32(at + 0xC, base_ + sample);
    }

    uint32_t PutCell(uint32_t at, const Cell& cell)
    {
        uint8_t flags = 0;
        if (cell.index >= 0)
        {
            flags |= cell.target ? kCellSlideTarget : kCellNote;
        }

        if (cell.volume >= 0)
        {
            flags |= kCellVolume;
        }

        if (cell.effect != 0)
        {
            flags |= uint8_t(4 * cell.effect) & kCellEffect;
        }

        bytes_[at++] = flags;
        if (cell.index >= 0)
        {
            bytes_[at++] = uint8_t(cell.index);
            bytes_[at++] = uint8_t(cell.index >> 8);
            bytes_[at++] = uint8_t(cell.instrument);
        }

        if (cell.volume >= 0)
        {
            bytes_[at++] = uint8_t(cell.volume);
        }

        if (cell.effect != 0)
        {
            bytes_[at++] = uint8_t(cell.parameter);
        }

        return at;
    }

    uint32_t base_;
    std::vector<uint8_t> bytes_;
};

} // namespace agblib::testing::frontline_mod_cart
