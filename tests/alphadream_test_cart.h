// SPDX-License-Identifier: MIT

// A small synthetic cartridge of AlphaDream's driver for the tests: the routines that detection reads, the tables, an
// instrument, a sample and two songs.

#pragma once

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "agblib/agb/rom.h"

namespace agblib::testing::alphadream_test_cart
{

using agblib::agb::kRomBase;
using agblib::agb::Rom;

// Addresses in a test cartridge: the routines that detection finds, then the tables.
constexpr uint32_t kStart = kRomBase + 0x100;
constexpr uint32_t kLookup = kRomBase + 0x200;
constexpr uint32_t kVoice = kRomBase + 0x280;
constexpr uint32_t kStep = kRomBase + 0x300;
constexpr uint32_t kPsgPitch = kRomBase + 0x400;
constexpr uint32_t kNoise = kRomBase + 0x480;
constexpr uint32_t kWave = kRomBase + 0x4C0;
constexpr uint32_t kNote = kRomBase + 0x500;
constexpr uint32_t kCommands = kRomBase + 0x540;
constexpr uint32_t kCommandTable = kRomBase + 0x580;
constexpr uint32_t kEffectHandler = kRomBase + 0x5C0;
constexpr uint32_t kEffectStart = kRomBase + 0x600;
constexpr uint32_t kWaveLevel = kRomBase + 0x640;
constexpr uint32_t kSongTable = kRomBase + 0x800;
constexpr uint32_t kInstruments = kRomBase + 0x900;
constexpr uint32_t kSamples = kRomBase + 0xA00;
constexpr uint32_t kPsgPitchTable = kRomBase + 0xB00;
constexpr uint32_t kNoiseTable = kRomBase + 0xC00;
constexpr uint32_t kWaveTable = kRomBase + 0xC80;
constexpr uint32_t kWaveLevels = kRomBase + 0xD00;
constexpr uint32_t kSong = kRomBase + 0x1000;

// The routines' code, as driver.cpp's patterns give it, with each x as 0.
constexpr const char* kStartCode =
    "B5F0 4647 B480 1C04 1C0D F000 F800 1C20 214B F000 F800 4E00 6831 4B00 18C9 1909 "
    "780A 2001 4310 7008 6831 18C9 1909 780A 20FD 4010 7008 4800";

// The Hamtaro games' version of the routine that starts a song: it takes the song in r4 and the player in r5, and it
// makes one call before it sets the player's flags (0x0814AC70 in A84J).
constexpr const char* kHamtaroStartCode =
    "B5F0 4647 B480 1C05 1C0C F000 F800 4E00 6831 4B00 18C9 1949 780A 2001 4310 7008 6831 18C9 1949 780A 20FD 4010 "
    "7008 4800";

constexpr const char* kLookupCode =
    "B500 1C0B 4A00 0041 1889 8809 1889 3001 0040 1880 8800 1882 4291 D000 7808 4298 "
    "DC00 7848 4298 DB00 1C08 E000";
constexpr const char* kVoiceCode = "B5F0 4657 464D 4643 B4A8 8872 4900 0450 0BC0 580F 187F 0452 D300 6878 213C 2200";
constexpr const char* kStepCode = "4684 A31A 5CC9 A30C 0708 0E80 18C0 6803 6840 1AC0 4350 0A00 181B 0909 40CB 4A00";
constexpr const char* kPsgPitchCode =
    "B510 0400 0E03 21FF 0409 4001 0C0C 3B24 2B00 DB00 4A00 0058 1880 8801 1C58 0040 "
    "1880 8800 1A40 4360 0A00 1809 2280 0112";
constexpr const char* kNoiseCode = "B500 1C01 2914 DC00 20D7 E000 2950 DC00 4800 3915";
constexpr const char* kWaveCode = "0100 4900 1840 4B00 21C0 7019";
constexpr const char* kWaveLevelCode = "4900 7918 0880 1840 7800 0200 8010";

// The start of a note in the routine that runs a track: A88E's, then B85A's and A84J's. Those two read a volume after
// the key. A84J's then calls the track's volume handler.
constexpr const char* kNoteCode = "8928 1C41 8129 0400 0C00 1810 7804 207F 4020 72E8 8829 2040 4008";
constexpr const char* kHamSportsNoteCode =
    "8928 1C41 8129 0400 0C00 1810 7804 207F 4020 72E8 1C48 8128 0409 0C09 1851 7808 7368 8829 2040 4008";
constexpr const char* kHamtaro4NoteCode =
    "8928 1C41 8129 0400 0C00 1810 7804 207F 4020 72E8 1C48 8128 0409 0C09 1851 7808 7368 6879 1C28";

// The routine that runs the game's commands, command 5's handler and the start of the routine that starts a sound
// effect. The handler calls that routine at +6.
constexpr const char* kCommandCode = "0B18 2809 D800 0080 4900 1840 6800 4687";
constexpr const char* kEffectHandlerCode = "4800 4003 1C18";
constexpr const char* kEffectStartCode = "B5F0 4800";

class Cartridge
{
public:
    Cartridge() : image_(0x2000, 0)
    {
    }

    void U8(uint32_t at, uint8_t v)
    {
        image_[at - kRomBase] = v;
    }

    void U16(uint32_t at, uint16_t v)
    {
        U8(at, uint8_t(v));
        U8(at + 1, uint8_t(v >> 8));
    }

    void U32(uint32_t at, uint32_t v)
    {
        U16(at, uint16_t(v));
        U16(at + 2, uint16_t(v >> 16));
    }

    void Bytes(uint32_t at, std::initializer_list<int> bytes)
    {
        for (int b : bytes)
        {
            U8(at++, uint8_t(b));
        }
    }

    // Writes code at `at`. Returns the address after it.
    uint32_t Code(uint32_t at, const char* code)
    {
        std::istringstream words(code);
        std::string word;
        uint32_t end = at;
        while (words >> word)
        {
            U16(end, uint16_t(std::stoul(word, nullptr, 16)));
            end += 2;
        }

        return end;
    }

    // Writes a Thumb bl at `at` that calls `target`.
    void Bl(uint32_t at, uint32_t target)
    {
        const uint32_t offset = target - (at + 4);
        U16(at, uint16_t(0xF000 | ((offset >> 12) & 0x7FF)));
        U16(at + 2, uint16_t(0xF800 | ((offset >> 1) & 0x7FF)));
    }

    // Writes a routine's code at `at`, with its literal load at offset `literal` loading `value` from a pool after the
    // code.
    void Routine(uint32_t at, const char* code, uint32_t literal, uint32_t value)
    {
        const uint32_t end = Code(at, code);
        const uint32_t pool = (end + 3) & ~3u;
        const uint32_t load = at + literal;
        U16(load, uint16_t(ReadU16(load) | ((pool - ((load + 4) & ~3u)) / 4)));
        U32(pool, value);
    }

    uint16_t ReadU16(uint32_t at) const
    {
        return xsflib::LoadLe16(&image_[at - kRomBase]);
    }

    Rom Build() const
    {
        Rom rom;
        rom.Assign(image_);
        return rom;
    }

private:
    std::vector<uint8_t> image_;
};

// A cartridge with the driver, one instrument of one region that plays one looping sample, and song 1, whose track 2
// plays the sample and whose track 8 plays square 1. Song 0 plays no track. `start_code` is the routine that starts a
// song, whose song table's literal load is at `start_literal`.
Cartridge TestCartridge(std::initializer_list<int> track2, const char* start_code = kStartCode,
                        uint32_t start_literal = 0x36)
{
    Cartridge c;
    c.Routine(kStart, start_code, start_literal, kSongTable);
    c.Routine(kLookup, kLookupCode, 4, kInstruments);
    c.Routine(kVoice, kVoiceCode, 0xC, kSamples);
    c.Routine(kStep, kStepCode, 0x1E, 0x0A080000);
    c.Routine(kPsgPitch, kPsgPitchCode, 0x14, kPsgPitchTable);
    c.Routine(kNoise, kNoiseCode, 0x10, kNoiseTable);
    c.Routine(kWave, kWaveCode, 2, kWaveTable);
    c.Routine(kWaveLevel, kWaveLevelCode, 0, kWaveLevels);
    c.Code(kNote, kNoteCode);

    // The NR32 settings of the three games: 25, 50, 75 and 100%.
    c.Bytes(kWaveLevels, {0x60, 0x40, 0x80, 0x20});

    // The step routine's tables, where its adr instructions point: 13 steps, then a byte for each key.
    for (int n = 0; n <= 12; n++)
    {
        c.U32(kStep + 0x38 + 4 * uint32_t(n), uint32_t(std::lround(8388608 * std::pow(2.0, n / 12.0))));
    }

    for (int key = 0; key < 128; key++)
    {
        c.U8(kStep + 0x6C + uint32_t(key), uint8_t((10 - key / 12) << 4 | key % 12));
    }

    for (int key = 0; key < 92; key++)
    {
        c.U16(kPsgPitchTable + 2 * uint32_t(key),
              uint16_t(std::lround(131072 / (65.40639 * std::pow(2.0, key / 12.0)))));
    }

    // Two songs, then the table's end.
    c.U32(kSongTable, kSong);
    c.U32(kSongTable + 4, kSong + 0x10);
    c.U32(kSongTable + 8, 0);
    c.U16(kSong, 0);
    c.U16(kSong + 0x10, 0x104); // tracks 2 and 8
    c.U16(kSong + 0x12, 0x10);
    c.U16(kSong + 0x14, 0x80);

    uint32_t at = kSong + 0x20;
    for (int b : track2)
    {
        c.U8(at++, uint8_t(b));
    }

    c.Bytes(kSong + 0x90, {0xF0, 0x01, 0xF1, 0xFF, 0xF2, 0x7F, 0x08, 0x48, 0xFF});

    // Instrument 0: keys 40-80 play sample 0, attack 0x40, decay 0x10, sustain 0x80, release 0x20. Instrument 1: a
    // square of duty 2 over every key.
    c.U16(kInstruments, 6);
    c.U16(kInstruments + 2, 14);
    c.U16(kInstruments + 4, 22);
    c.Bytes(kInstruments + 6, {40, 80, 0, 0, 0x40, 0x10, 0x80, 0x20});
    c.Bytes(kInstruments + 14, {0, 127, 2, 0, 0x10, 0x10, 0xF0, 0x10});

    // Sample 0: 64 points at 8000 Hz, looping from point 32.
    c.U32(kSamples, 4);
    c.U32(kSamples + 4, 0x40000000);
    c.U32(kSamples + 8, 8000 * 1024);
    c.U32(kSamples + 12, 32);
    c.U32(kSamples + 16, 64);
    for (uint32_t i = 0; i < 64; i++)
    {
        c.U8(kSamples + 20 + i, uint8_t(i % 2 ? 0xC0 : 0x40));
    }

    return c;
}

} // namespace agblib::testing::alphadream_test_cart
