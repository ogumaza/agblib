// SPDX-License-Identifier: MIT

// Definitions that the parts of the detection of Konami's driver share (driver.cpp, driver_songs.h and
// driver_samples.h): the limits of a scan, the names and the patterns they use, the code that loads a table, the
// records of a revision's code, FindLiteral() and IsEmptyEntry(). Only those files include this header. The definitions
// therefore keep internal linkage.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/thumb.h"
#include "agblib/konami/driver.h"
#include "agblib/konami/seqformat.h"

namespace agblib::konami
{

using agblib::agb::FindThumb;
using agblib::agb::Hex;
using agblib::agb::kRomBase;
using agblib::agb::kSecondCycles;
using agblib::agb::ThumbLiteral;
using agblib::agb::ThumbLiteralAddress;

namespace
{

using Pattern = agb::ThumbPattern;

// Limits on the commands and table entries that a scan reads. Unrelated ROM data then can't keep detection busy.
constexpr uint32_t kMaxScanCommands = 60000;
constexpr int kMaxSongs = 1024;

// Returns a pattern that matches one Thumb instruction exactly.
constexpr Pattern Exact(uint16_t v)
{
    return {v, 0xFFFF};
}

// Returns a pattern that matches an instruction by its high byte, leaving the immediate as a wildcard (an ldr literal
// offset or a branch).
constexpr Pattern HiByte(uint16_t v)
{
    return {v, 0xFF00};
}

// Code that loads one of the driver's tables, and the index of its ldr of the table.
struct TableCode
{
    std::vector<Pattern> code;
    int ldr = 0;
};

// The size of a candidate song table as CountSongs() finds it.
struct SongCount
{
    int entries = 0;     // table entries, including empty ones between songs
    int valid = 0;       // entries that contain a song
    int unreadable = -1; // the entry that ended the count, a song in the ROM whose tracks don't parse; -1 if none did
};

// How the play-song routine works out the address of a song's entry: it shifts the song number left by `first_shift`,
// adds the song number to it (or subtracts it if `subtracts` is set), and shifts the sum left by `second_shift`.
struct SongEntrySum
{
    int first_shift = 3;
    bool subtracts = false;
    int second_shift = 2;
};

// The code that loads each of a revision's tables, and the sum that gives a song's entry.
struct RevisionCode
{
    TableCode song_start;    // code in the play-song routine that loads the song table
    SongEntrySum song_entry; // the play-song routine's sum for a song's entry
    TableCode note_start;    // code in the note start that loads the sample table
    TableCode volume;        // code that loads the volume table
    TableCode psg_freq;      // code that loads the PSG frequency table
    TableCode noise;         // code that loads the noise table
};

// Returns the literal loaded by the ldr at index `ldr_index` of the first match of `pat` whose literal is a ROM
// address, or 0 if there's none.
uint32_t FindLiteral(const Rom& rom, const std::vector<Pattern>& pat, int ldr_index)
{
    for (uint32_t hit : FindThumb(rom, pat))
    {
        const uint32_t v = ThumbLiteral(rom, hit + 2 * ldr_index);
        if (rom.Contains(v))
        {
            return v;
        }
    }

    return 0;
}

// Returns true for an empty (all-zero) song table entry.
bool IsEmptyEntry(const SongHeader& h)
{
    if (h.base != 0)
    {
        return false;
    }

    for (uint16_t o : h.offsets)
    {
        if (o)
        {
            return false;
        }
    }

    return true;
}

} // namespace
} // namespace agblib::konami
