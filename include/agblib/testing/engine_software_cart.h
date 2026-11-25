// SPDX-License-Identifier: MIT

// Synthetic data for tests of Engine Software's driver: a bank of four instruments and three songs, and the tables
// that the driver reads (the sine and the pitch table). The instruments have samples that loop and that play once,
// volume envelopes with a sustain point and with a loop, and a pan envelope. The first song's rows use every effect
// that the driver handles and every command of the volume column, with notes, key offs, slides to a note, a delayed
// note, an empty row, a pattern delay, a break, a jump and the return to the restart order. The second song is a
// jingle, and the third starts at full volume.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"

namespace agblib::testing::engine_software_cart
{

using agblib::engine_software::Build;
using agblib::engine_software::kBankSongTable;
using agblib::engine_software::kInstrumentSamples;
using agblib::engine_software::kMiddleIndex;
using agblib::engine_software::kPitchHalfwords;
using agblib::engine_software::kPitchWords;
using agblib::engine_software::kRowFields;
using agblib::engine_software::SongStart;

// A channel's fields in a row. A field of 0 has no byte in the packed row.
struct Cell
{
    uint8_t note = 0;
    uint8_t instrument = 0;
    uint8_t volume = 0;
    uint8_t effect = 0;
    uint8_t operand = 0;
};

// A row: a cell for each channel, or none for an empty row.
using Row = std::vector<Cell>;
using Pattern = std::vector<Row>;

// An instrument: its header's fields, its samples, and its envelopes as their 4 header bytes and their points.
struct Instrument
{
    uint32_t loop_start = 0;
    uint32_t loop_bytes = 0;
    uint8_t volume = 64;
    uint8_t pan = 0x80;
    int8_t fine_tune = 0;
    int8_t relative = 0;
    uint32_t fade_out = 0x80;
    std::vector<uint8_t> volume_envelope = {0, 0xFF, 0xFF, 0xFF};
    std::vector<uint16_t> volume_points;
    std::vector<uint8_t> pan_envelope = {0, 0xFF, 0xFF, 0xFF};
    std::vector<uint16_t> pan_points;
    std::vector<int8_t> samples;
};

struct Song
{
    uint8_t channels = 0;
    uint8_t restart = 0;
    uint8_t speed = 0;
    uint8_t tempo = 0;
    std::vector<uint8_t> orders;
    std::vector<Pattern> patterns;
};

// The bank's kind byte, as in the games' banks.
constexpr uint8_t kKind = 0x21;

// The first half of the vibrato's sine, ProTracker's table of 64 halfwords. The second half is the first negated.
constexpr int16_t kSineHalf[32] = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
                                   255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

// The pitch table's entries in all: halfwords, then words.
constexpr uint32_t kPitchEntries = kPitchHalfwords + kPitchWords;

// Where the bank goes in the synthetic image.
constexpr uint32_t kBankAddress = agblib::agb::kRomBase + 0x60000;

// Returns the pitch table's entry at `index`: the rate in hertz, 8,363 at kMiddleIndex and doubling every 1,536.
inline uint32_t PitchAt(uint32_t index)
{
    return uint32_t(std::floor(8363.0 * std::exp2((double(index) - kMiddleIndex) / 1536.0)));
}

// Returns `count` signed points of a triangle, a square or a decaying noise.
inline std::vector<int8_t> Triangle(int count)
{
    std::vector<int8_t> v;
    for (int i = 0; i < count; i++)
    {
        const int phase = (i * 512 / count) % 512;
        v.push_back(int8_t(phase < 256 ? phase - 128 : 383 - phase));
    }

    return v;
}

inline std::vector<int8_t> Square(int count)
{
    std::vector<int8_t> v;
    for (int i = 0; i < count; i++)
    {
        v.push_back(int8_t(i < count / 2 ? 100 : -100));
    }

    return v;
}

inline std::vector<int8_t> Noise(int count)
{
    std::vector<int8_t> v;
    uint32_t state = 12345;
    for (int i = 0; i < count; i++)
    {
        state = state * 1103515245u + 12345u;
        const int level = 127 - 127 * i / count;
        v.push_back(int8_t((int((state >> 16) & 0xFF) - 128) * level / 127));
    }

    return v;
}

// The instruments: 1 a triangle that loops, 2 a noise that plays once, 3 a square with a sustained volume envelope and
// a pan envelope, 4 a triangle with a looped volume envelope.
inline std::vector<Instrument> Instruments()
{
    Instrument looped;
    looped.loop_start = 8;
    looped.loop_bytes = 56;
    looped.volume = 48;
    looped.pan = 0x40;
    looped.samples = Triangle(64);

    Instrument once;
    once.pan = 0xC0;
    once.fine_tune = -20;
    once.relative = 5;
    once.fade_out = 0x200;
    once.samples = Noise(200);

    Instrument sustained;
    sustained.loop_bytes = 32;
    sustained.volume = 40;
    sustained.fade_out = 0x1000;
    sustained.volume_envelope = {4, 2, 0xFF, 0xFF};
    sustained.volume_points = {0, 64, 3, 20, 8, 48, 16, 0};
    sustained.pan_envelope = {2, 0xFF, 0xFF, 0xFF};
    sustained.pan_points = {0, 0, 10, 64};
    sustained.samples = Square(32);

    Instrument cycled;
    cycled.loop_start = 16;
    cycled.loop_bytes = 32;
    cycled.volume = 56;
    cycled.relative = -12;
    cycled.fine_tune = 30;
    cycled.volume_envelope = {4, 0xFF, 1, 3};
    cycled.volume_points = {0, 10, 4, 64, 9, 30, 14, 60};
    cycled.samples = Triangle(48);

    return {looped, once, sustained, cycled};
}

// The first song: 4 channels, every effect and every command of the volume column.
inline Song EffectsSong()
{
    Song s;
    s.channels = 4;
    s.restart = 1;
    s.speed = 3;
    s.tempo = 140;
    s.orders = {0, 1, 2, 2};

    // The effects of the row and of the ticks after it, the volume column's slides and pans, a key off with and
    // without a volume envelope, a delayed note, retriggers, the music volume and its slide, and a break.
    s.patterns.push_back({
        {{49, 1, 0x40}, {37, 3, 0, 4, 0x46}, {61, 2}, {25, 4, 0x30, 8, 0x20}},
        {{0, 0, 0, 0, 0x37}, {}, {0, 0, 0, 0xA, 0x04}, {}},
        {{0, 0, 0, 1, 0x08}, {0, 0, 0, 4, 0}, {}, {0, 0, 0, 4, 0x33}},
        {},
        {{54, 0, 0, 3, 0x10}, {0x61}, {}, {0, 0, 0, 7, 0}},
        {{0, 0, 0, 3, 0}, {}, {0, 0, 0x65}, {0, 0, 0, 7, 0}},
        {{0, 0, 0, 5, 0x20}, {}, {0, 0, 0x75}, {}},
        {{0, 0, 0, 2, 0x04}, {42, 3}, {0, 0, 0x83}, {}},
        {{0, 0, 0, 6, 0x02}, {}, {0, 0, 0x92}, {0, 0, 0xC4}},
        {{0, 0, 0, 0xC, 0x28}, {0, 0, 0, 0x15, 0x05}, {}, {0, 0, 0xD3}},
        {{0, 0, 0, 0xE, 0x52}, {}, {61, 2}, {0, 0, 0xE5}},
        {{0, 0, 0, 0xE, 0xA3}, {0, 0, 0, 0x14, 0}, {0, 0, 0, 0xE, 0xB4}, {}},
        {{52, 1, 0, 0xE, 0xD2}, {}, {}, {0, 0, 0, 0x1B, 0x02}},
        {{0, 0, 0, 0x10, 0xC0}, {}, {}, {0, 0, 0, 0x1B, 0x02}},
        {{0, 0, 0, 0x11, 0x30}, {}, {0, 0, 0, 0x11, 0x05}, {}},
        {{0, 0, 0, 0xF, 0x04}, {0, 0, 0, 0xD, 0x02}, {}, {}},
    });

    // A tempo, an instrument without a note, a pattern delay, a sample offset, a note without an instrument with and
    // without an effect, effects that call the game's callback, and a jump.
    s.patterns.push_back({
        {{37, 4}, {44, 1, 0x50}, {0, 3}, {0, 0, 0, 0xF, 0x90}},
        {{0, 0, 0, 0xE, 0xE1}, {44, 1, 0, 9, 0x01}, {}, {}},
        {{0x61}, {}, {49, 0, 0, 4, 0x24}, {}},
        {{}, {44}, {}, {30, 2, 0x20}},
        {{0, 0, 0, 0xE, 0x02}, {}, {0, 0, 0, 0x13, 0x10}, {}},
        {},
        {{}, {0, 0, 0, 0xA, 0x30}, {}, {0, 0, 0, 1, 0x02}},
        {{}, {}, {0, 0, 0, 0xB, 0x03}, {}},
    });

    // A volume slide on a row with a volume, a pan of 255, and a release whose fade-out ends the note.
    s.patterns.push_back({
        {{49, 1, 0x10}, {51, 4}, {}, {56, 3}},
        {{0, 0, 0x40, 0xA, 0x40}, {}, {}, {}},
        {{}, {0, 0, 0, 8, 0xFF}, {}, {}},
        {{}, {}, {}, {0x61}},
        {{}, {}, {61, 2}, {}},
        {},
    });
    return s;
}

// The second song: a jingle of 2 channels that plays once.
inline Song JingleSong()
{
    Song s;
    s.channels = 2;
    s.speed = 5;
    s.tempo = 125;
    s.orders = {0};
    s.patterns.push_back({
        {{49, 2}, {42, 1, 0x30}},
        {},
        {{0, 0, 0, 0xA, 0x02}, {}},
        {},
        {{56, 1}, {}},
        {},
        {},
        {{}, {0, 0, 0, 0, 0x47}},
    });
    return s;
}

// The third song: 3 channels at speed 2. The game starts it at full volume.
inline Song ShortSong()
{
    Song s;
    s.channels = 3;
    s.speed = 2;
    s.tempo = 200;
    s.orders = {0, 0};
    s.patterns.push_back({
        {{49, 4}, {37, 1}, {61, 3}},
        {},
        {{}, {40, 0, 0, 3, 0x40}, {}},
        {},
    });
    return s;
}

// How the game starts each song.
inline SongStart Start(int song)
{
    switch (song)
    {
    case 1:
        return {.jingle = true};
    case 2:
        return {.frames = 1};
    default:
        return {.frames = 0x40};
    }
}

// The bank: its header, its instruments, its rows and its songs. Its offsets count from its start, wherever it goes.
class Bank
{
public:
    Bank();

    const std::vector<uint8_t>& Bytes() const
    {
        return bytes_;
    }

private:
    // Makes the bank at least `end` bytes long.
    void Grow(uint32_t end);
    void Put8(uint32_t at, uint8_t v);
    void Put16(uint32_t at, uint16_t v);
    void Put32(uint32_t at, uint32_t v);
    uint32_t AddRow(const Row& row, uint8_t channels);

    std::vector<uint8_t> bytes_;
};

inline void Bank::Grow(uint32_t end)
{
    if (bytes_.size() < end)
    {
        bytes_.resize(end, 0);
    }
}

inline void Bank::Put8(uint32_t at, uint8_t v)
{
    Grow(at + 1);
    bytes_[at] = v;
}

inline void Bank::Put16(uint32_t at, uint16_t v)
{
    Grow(at + 2);
    xsflib::StoreLe16(&bytes_[at], v);
}

inline void Bank::Put32(uint32_t at, uint32_t v)
{
    Grow(at + 4);
    xsflib::StoreLe32(&bytes_[at], v);
}

// Packs a row at the end of the bank and returns its offset: a bit for each field of each channel, the first channel
// in the high bit, then a byte for each set bit.
inline uint32_t Bank::AddRow(const Row& row, uint8_t channels)
{
    const uint32_t at = uint32_t(bytes_.size());
    const uint32_t bit_bytes = (kRowFields * uint32_t(channels) + 7) / 8;
    std::vector<uint8_t> bits(bit_bytes, 0);
    std::vector<uint8_t> values;
    uint32_t place = 0;
    for (uint8_t c = 0; c < channels; c++)
    {
        const Cell cell = c < row.size() ? row[c] : Cell();
        for (const uint8_t field : {cell.note, cell.instrument, cell.volume, cell.effect, cell.operand})
        {
            if (field != 0)
            {
                bits[place / 8] = uint8_t(bits[place / 8] | (0x80 >> (place % 8)));
                values.push_back(field);
            }

            place++;
        }
    }

    for (size_t i = 0; i < bits.size(); i++)
    {
        Put8(at + uint32_t(i), bits[i]);
    }

    for (size_t i = 0; i < values.size(); i++)
    {
        Put8(at + bit_bytes + uint32_t(i), values[i]);
    }

    return at;
}

inline Bank::Bank()
{
    const std::vector<Instrument> instruments = Instruments();
    const std::vector<Song> songs = {EffectsSong(), JingleSong(), ShortSong()};
    Put8(0, kKind);
    Put8(1, 1);
    Put8(2, uint8_t(instruments.size()));
    Put8(3, uint8_t(songs.size()));

    // The instruments after the song table, each with its samples, at a word.
    uint32_t at = kBankSongTable + 4 * uint32_t(songs.size());
    for (const Instrument& inst : instruments)
    {
        Put32(at, uint32_t(inst.samples.size()));
        Put32(at + 4, inst.loop_start);
        Put32(at + 8, inst.loop_bytes);
        Put8(at + 0x0C, inst.volume);
        Put8(at + 0x0D, inst.pan);
        Put8(at + 0x0E, uint8_t(inst.fine_tune));
        Put8(at + 0x0F, uint8_t(inst.relative));
        Put32(at + 0x10, inst.fade_out);
        const auto put_envelope =
            [&](uint32_t from, const std::vector<uint8_t>& head, const std::vector<uint16_t>& points)
        {
            for (size_t i = 0; i < head.size(); i++)
            {
                Put8(from + uint32_t(i), head[i]);
            }

            for (size_t i = 0; i < points.size(); i++)
            {
                Put16(from + 4 + 2 * uint32_t(i), points[i]);
            }
        };
        put_envelope(at + 0x14, inst.volume_envelope, inst.volume_points);
        put_envelope(at + 0x48, inst.pan_envelope, inst.pan_points);
        for (size_t i = 0; i < inst.samples.size(); i++)
        {
            Put8(at + kInstrumentSamples + uint32_t(i), uint8_t(inst.samples[i]));
        }

        at += (uint32_t(inst.samples.size()) + kInstrumentSamples + 3) & ~3u;
        Grow(at);
    }

    // The rows of every song, then the songs.
    std::vector<std::vector<std::vector<uint32_t>>> offsets;
    for (const Song& s : songs)
    {
        offsets.emplace_back();
        for (const Pattern& p : s.patterns)
        {
            offsets.back().emplace_back();
            for (const Row& row : p)
            {
                offsets.back().back().push_back(row.empty() ? 0 : AddRow(row, s.channels));
            }
        }
    }

    for (size_t n = 0; n < songs.size(); n++)
    {
        const Song& s = songs[n];
        at = (uint32_t(bytes_.size()) + 3) & ~3u;
        Put32(kBankSongTable + 4 * uint32_t(n), at);
        Put8(at, s.channels);
        Put8(at + 1, uint8_t(s.orders.size()));
        Put8(at + 2, s.restart);
        Put8(at + 3, uint8_t(s.patterns.size()));
        Put8(at + 4, s.speed);
        Put8(at + 5, s.tempo);
        Put16(at + 6, 0);
        for (size_t o = 0; o < s.orders.size(); o++)
        {
            Put8(at + 8 + uint32_t(o), s.orders[o]);
        }

        uint32_t pattern = at + 8 + ((uint32_t(s.orders.size()) + 3) & ~3u);
        for (size_t p = 0; p < s.patterns.size(); p++)
        {
            Put16(pattern, uint16_t(s.patterns[p].size()));
            Put16(pattern + 2, 0);
            for (size_t r = 0; r < s.patterns[p].size(); r++)
            {
                Put32(pattern + 4 + 4 * uint32_t(r), offsets[n][p][r]);
            }

            pattern += 4 + 4 * uint32_t(s.patterns[p].size());
        }
    }
}

// Returns a synthetic cartridge with the tables of `build` at their addresses and the bank at kBankAddress.
inline std::vector<uint8_t> Image(const Build& build)
{
    using agblib::agb::kRomBase;
    const Bank bank;
    std::vector<uint8_t> bytes(kBankAddress - kRomBase, 0);
    bytes.insert(bytes.end(), bank.Bytes().begin(), bank.Bytes().end());

    for (uint32_t i = 0; i < 64; i++)
    {
        const int16_t sine = i < 32 ? kSineHalf[i] : int16_t(-kSineHalf[i - 32]);
        xsflib::StoreLe16(&bytes[build.sine - kRomBase + 2 * i], uint16_t(sine));
    }

    for (uint32_t i = 0; i < kPitchEntries; i++)
    {
        if (i < kPitchHalfwords)
        {
            xsflib::StoreLe16(&bytes[build.pitch - kRomBase + 2 * i], uint16_t(PitchAt(i)));
        }
        else
        {
            xsflib::StoreLe32(&bytes[build.pitch_words - kRomBase + 4 * (i - kPitchHalfwords)], PitchAt(i));
        }
    }

    return bytes;
}

// Returns the driver as the model reads it from the synthetic image: the build of `build`, the cart's bank and each
// song's start.
inline agblib::engine_software::DriverInfo TestInfo(const agblib::agb::Rom& rom, const Build& build)
{
    agblib::engine_software::DriverInfo info;
    info.build = build;
    std::string error;
    agblib::engine_software::ReadBank(rom, kBankAddress, info.bank, info.songs, error);
    for (size_t s = 0; s < info.songs.size(); s++)
    {
        info.songs[s].start = Start(int(s));
    }

    return info;
}

} // namespace agblib::testing::engine_software_cart
