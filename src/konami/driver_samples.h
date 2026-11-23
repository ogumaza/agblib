// SPDX-License-Identifier: MIT

// The part of the detection of Konami's driver that reads samples: the sample headers of each revision, the samples
// that the songs play, and the search for the sample table. Only driver.cpp includes this header. Its definitions
// therefore keep internal linkage.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/thumb.h"
#include "agblib/konami/driver.h"
#include "agblib/konami/seqformat.h"
#include "driver_internal.h"

namespace agblib::konami
{
namespace
{

// Where a packed sample's data starts: past its header and its table of pairs.
constexpr uint32_t kPackedData = kSampleHeaderBytes + kPackedPairBytes;

// Returns true if `p` could point at a sample header: a sane step, data that fits in the ROM, and a loop start inside
// the sample or -1.
bool PlausibleSampleHeader(const Rom& rom, uint32_t p)
{
    if (!rom.Contains(p, kSampleHeaderBytes))
    {
        return false;
    }

    const uint32_t step = rom.U32(p);
    const int32_t len = rom.S32(p + 4);
    const int32_t loop = rom.S32(p + 8);

    if (step < 0x40 || step > 0x40000)
    {
        return false;
    }
    if (len <= 0 || len > 0x1000000)
    {
        return false;
    }
    if (!rom.Contains(p + kSampleHeaderBytes, uint32_t(len)))
    {
        return false;
    }
    if (loop != -1 && (loop < 0 || loop >= len))
    {
        return false;
    }

    return true;
}

// Returns true if `entry` could be an entry of the Dungeon Dice Monsters revision's sample table: a pointer to the PCM,
// and a word with the length in blocks of 16 samples in bits 20-31 and the sample's timer period in bits 0-15, for a
// rate of 1 to 65 kHz.
bool PlausibleDungeonDiceSample(const Rom& rom, uint32_t entry)
{
    if (!rom.Contains(entry, 8))
    {
        return false;
    }

    const uint32_t word = rom.U32(entry + 4);
    const uint32_t length = (word >> 20) * 16;
    const uint32_t period = word & 0xFFFF;
    return length > 0 && period >= 0x100 && period <= 0x4000 && rom.Contains(rom.U32(entry), length);
}

// Returns the header of sample `index` in a table of pointers to sample headers at `table`, with valid = false if
// the model can't read it.
SampleInfo SampleHeaderAt(const Rom& rom, uint32_t table, int index)
{
    SampleInfo s;
    const uint32_t p = rom.U32(table + 4 * uint32_t(index));
    if (!PlausibleSampleHeader(rom, p))
    {
        return s;
    }

    s.valid = true;
    s.step = rom.U32(p);
    s.length = rom.S32(p + 4);
    s.loop_start = rom.S32(p + 8);
    s.data = p + kSampleHeaderBytes;

    return s;
}

// Returns entry `index` of the Dungeon Dice Monsters revision's sample table at `table`, with valid = false if the
// model can't read it. The revision's samples have no loop.
SampleInfo DungeonDiceSampleAt(const Rom& rom, uint32_t table, int index)
{
    SampleInfo s;
    const uint32_t entry = table + 8 * uint32_t(index);
    if (PlausibleDungeonDiceSample(rom, entry))
    {
        const uint32_t word = rom.U32(entry + 4);
        s.valid = true;
        s.period = word & 0xFFFF;
        s.length = int32_t((word >> 20) * 16);
        s.data = rom.U32(entry);
    }

    return s;
}

// Returns the header of packed sample `index` in the table of pointers to sample headers at `table`, with valid = false
// if the sample isn't packed or the model can't read it. The note start takes the length's low 24 bits. The mixer can't
// loop a packed sample, and the header has to give no loop.
SampleInfo PackedSampleAt(const Rom& rom, uint32_t table, int index)
{
    SampleInfo s;
    const uint32_t p = rom.U32(table + 4 * uint32_t(index));
    if (!rom.Contains(p, kPackedData + 1))
    {
        return s;
    }

    const uint32_t step = rom.U32(p);
    const uint32_t length = rom.U32(p + 4);
    if (!(length & 0x80000000) || !(length & 0xFFFFFF) || step < 0x40 || step > 0x40000 || rom.S32(p + 8) != -1)
    {
        return s;
    }

    s.valid = true;
    s.packed = true;
    s.step = step;
    s.length = int32_t(length & 0xFFFFFF);
    s.data = p + kPackedData;

    return s;
}

// Returns sample `index` of revision `revision`'s sample table at `table`, with valid = false if the model can't read
// it.
SampleInfo SampleAt(const Rom& rom, uint32_t table, int index, Revision revision)
{
    if (revision == Revision::kDungeonDiceMonsters)
    {
        return DungeonDiceSampleAt(rom, table, index);
    }

    if (LaterUltimateMasters(revision))
    {
        if (const SampleInfo s = PackedSampleAt(rom, table, index); s.valid)
        {
            return s;
        }
    }

    return SampleHeaderAt(rom, table, index);
}

// Returns the PCM of packed sample `s`. The mixer plays a packed sample from a stream of codes. A byte from C0 to 7F
// is a sample. A byte from A0 to BF plays pair n - A0 of the table before the data: the pair's high byte, then its low
// byte. A byte from 90 to 9F plays n zero samples, and a byte from 80 to 8F plays n samples of the value in the byte
// after n. In both, n is the byte's low nibble, or the next byte if the nibble is 0. This follows the mixer as it moves
// on by one sample of the data for each sample of the mix.
std::vector<int8_t> UnpackSample(const Rom& rom, const SampleInfo& s)
{
    const uint32_t pairs = s.data - kPackedPairBytes;
    uint32_t p = s.data;
    int8_t value = 0; // the value that a run or pair plays next
    int count = 0;    // how many samples of the run or pair are left, the one that plays included

    // Returns the next sample of the stream after skipping `skip` single samples. A run or a pair ends the skip.
    auto fetch = [&](int skip) -> int8_t
    {
        for (;;)
        {
            const int8_t b = rom.S8(p++);
            if (b >= -0x40)
            {
                if (--skip >= 0)
                {
                    continue;
                }

                p--; // the mixer leaves the pointer at the sample that plays
                return b;
            }

            if (b >= -0x60)
            {
                const uint16_t pair = rom.U16(pairs + 2 * uint32_t(b + 0x60));
                value = int8_t(pair & 0xFF);
                count = 2;
                return int8_t(pair >> 8);
            }

            count = b & 0x0F;
            if (!count)
            {
                count = rom.U8(p++);
            }

            value = b >= -0x70 ? 0 : rom.S8(p++);
            return value;
        }
    };

    std::vector<int8_t> pcm;
    pcm.reserve(size_t(s.length));
    pcm.push_back(fetch(0));
    while (pcm.size() < size_t(s.length))
    {
        if (count > 1)
        {
            count--;
            pcm.push_back(value);
            continue;
        }

        // After a single sample, the pointer is still at it. After a run or pair, it's at the next code.
        const int skip = count ? 0 : 1;
        count = 0;
        pcm.push_back(fetch(skip));
    }

    return pcm;
}

// Returns true if `used` is nonempty and every referenced sample has a plausible header at `table`.
bool SampleTableFits(const Rom& rom, uint32_t table, const std::vector<int>& used)
{
    for (int idx : used)
    {
        if (!SampleHeaderAt(rom, table, idx).valid)
        {
            return false;
        }
    }

    return !used.empty();
}

// Returns the samples the songs play, the most played first. In the Dungeon Dice Monsters revision, a note gives an
// entry of the sample map at `sample_map`.
std::vector<int> UsedSamples(const Rom& rom, uint32_t song_table, int song_count, Revision revision,
                             uint32_t sample_map)
{
    std::map<int, int> freq;
    auto count_note = [&](const Command& c, uint32_t)
    {
        if (c.op != Op::kNote && c.op != Op::kNextNote)
        {
            return;
        }

        if (revision != Revision::kDungeonDiceMonsters)
        {
            freq[c.sample]++;
        }
        else if (rom.Contains(sample_map + uint32_t(c.sample)))
        {
            freq[rom.U8(sample_map + uint32_t(c.sample))]++;
        }
    };

    for (int s = 0; s < song_count; s++)
    {
        SongHeader h;
        if (!ReadSongHeader(rom, song_table, s, revision, h) || IsEmptyEntry(h))
        {
            continue;
        }

        for (int t = kPsgTracks; t < h.tracks; t++)
        {
            WalkTrack(rom, h, t, revision, count_note);
        }
    }

    std::vector<int> used;
    for (const auto& [index, count] : freq)
    {
        used.push_back(index);
    }

    // Check the most frequently used samples first to reject invalid candidates sooner.
    std::sort(used.begin(), used.end(), [&](int a, int b) { return freq.at(a) > freq.at(b); });

    return used;
}

// Scans for a table containing the samples in `used`. `used` must not be empty. Returns 0 if none is found. Adds a
// warning if the table could only be identified by a guess.
uint32_t ScanForSampleTable(const Rom& rom, const std::vector<int>& used, std::vector<std::string>& log,
                            std::vector<std::string>& warnings)
{
    // The sample headers follow the table directly. Entry 0 therefore points just past the last entry, and the entries
    // increase. That pins down the start.
    const uint32_t max_used = uint32_t(*std::max_element(used.begin(), used.end()));
    for (uint32_t t = kRomBase; t + 4 <= rom.End(); t += 4)
    {
        const uint32_t first = rom.U32(t);
        if (first <= t || (first - t) % 4 || !rom.Contains(first))
        {
            continue;
        }

        const uint32_t n = (first - t) / 4;
        if (n <= max_used || n > 0x10000)
        {
            continue;
        }

        bool ok = true;
        for (uint32_t i = 0, prev = 0; i < n && ok; i++)
        {
            const uint32_t p = rom.U32(t + 4 * i);
            ok = p > prev && PlausibleSampleHeader(rom, p);
            prev = p;
        }

        if (ok)
        {
            log.push_back("sample table " + Hex(t) + " (from data scan)");
            return t;
        }
    }

    // Rips zero the entries of unused samples, and that breaks the pattern above. Accept the first table that serves
    // every sample the songs use.
    for (uint32_t t = kRomBase; t + 4 <= rom.End(); t += 4)
    {
        if (SampleTableFits(rom, t, used))
        {
            warnings.push_back("sample table " + Hex(t) + " guessed from the data; check the result");
            return t;
        }
    }

    return 0;
}

// Finds the sample table of revision `revision` from the note-start routine's code, or failing that in the data. A
// table in the data has to be in the newer revisions' layout. Returns 0 if no song plays a sample, and 0 with `error`
// set if there's no table it can use. Adds the samples it can't read, or a table that's only a guess, to `warnings`.
// The messages name `program`.
uint32_t FindSampleTable(const Rom& rom, const std::vector<int>& used, const TableCode& note_start, Revision revision,
                         const std::string& program, std::vector<std::string>& log, std::vector<std::string>& warnings,
                         std::string& error)
{
    if (used.empty())
    {
        log.push_back("no song plays a sample, so no sample table is needed");
        return 0;
    }

    // Of the tables the code loads, take the one with the fewest samples the model can't read.
    const uint32_t ldr_offset = 2 * uint32_t(note_start.ldr);
    const std::vector<uint32_t> hits = FindThumb(rom, note_start.code);
    uint32_t best = 0;
    std::vector<int> unreadable; // samples used by the songs that have no readable header in `best`
    for (uint32_t hit : hits)
    {
        const uint32_t t = ThumbLiteral(rom, hit + ldr_offset);
        if (!rom.Contains(t))
        {
            continue;
        }

        std::vector<int> missing;
        for (int idx : used)
        {
            if (!SampleAt(rom, t, idx, revision).valid)
            {
                missing.push_back(idx);
            }
        }

        if (!best || missing.size() < unreadable.size())
        {
            best = t;
            unreadable = missing;
        }
    }

    // The code gives the table's address. A table found in the data instead would be the wrong one. The table only
    // has to have a header the model can read for at least half of the samples. The others may be in a mode that the
    // model doesn't support. A negative length is one such mode.
    if (best)
    {
        if (unreadable.size() * 2 > used.size())
        {
            error = "found the note-start code, but " + program +
                    " can't read most of the songs' samples in the sample table it loads, at " + Hex(best) +
                    " (--sample-table overrides detection)";
            return 0;
        }

        log.push_back("sample table " + Hex(best) + " (from note-start code)");
        if (!unreadable.empty())
        {
            std::sort(unreadable.begin(), unreadable.end());
            std::string list;
            for (int idx : unreadable)
            {
                list += (list.empty() ? "" : ", ") + std::to_string(idx);
            }

            warnings.push_back(program + " can't read these samples and skips their notes: " + list);
        }

        return best;
    }

    const char* why = hits.empty() ? "note-start code not recognised"
                                   : "the sample table that the note-start code loads isn't in the ROM";
    if (revision == Revision::kDungeonDiceMonsters)
    {
        error = std::string(why) + ", and " + program +
                " can't look for this revision's sample table in the data (--sample-table overrides detection)";
        return 0;
    }

    log.push_back(std::string(why) + ", scanning data for the sample table");
    const uint32_t scanned = ScanForSampleTable(rom, used, log, warnings);
    if (!scanned)
    {
        error = "couldn't find the sample table (--sample-table overrides detection)";
    }

    return scanned;
}

} // namespace
} // namespace agblib::konami
