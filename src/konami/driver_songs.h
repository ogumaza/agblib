// SPDX-License-Identifier: MIT

// The part of the detection of Konami's driver that finds the song table: the checks of a candidate table's songs and
// layout, the code of each revision that loads the table, and the scan of the data. Only driver.cpp includes this
// header. Its definitions therefore keep internal linkage.

#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "agblib/agb/rom.h"
#include "agblib/agb/thumb.h"
#include "agblib/konami/driver.h"
#include "agblib/konami/seqformat.h"
#include "driver_internal.h"

namespace agblib::konami
{
namespace
{

// Returns true if a song table entry points into the ROM: the song's base address and the start of each of its tracks.
bool HeaderInRom(const Rom& rom, const SongHeader& h)
{
    if (!rom.Contains(h.base))
    {
        return false;
    }

    for (uint16_t o : h.offsets)
    {
        if (!rom.Contains(h.base + o))
        {
            return false;
        }
    }

    return true;
}

// Returns the number of the song's tracks that parse cleanly to an end command, or -1 if the header doesn't point into
// the ROM.
int ScoreSong(const Rom& rom, const SongHeader& h, Revision revision)
{
    if (!HeaderInRom(rom, h))
    {
        return -1;
    }

    int good = 0;
    for (int t = 0; t < h.tracks; t++)
    {
        if (WalkTrack(rom, h, t, revision, [](const Command&, uint32_t) {}, kMaxScanCommands))
        {
            good++;
        }
    }

    return good;
}

// Counts the entries of a candidate song table. An entry contains a song if at least half of its tracks parse cleanly
// to an end. Song data follows the table. The table therefore ends where the lowest song data begins.
SongCount CountSongs(const Rom& rom, uint32_t table, Revision revision)
{
    const uint32_t entry_size = SongEntrySize(revision);
    uint32_t data_start = 0xFFFFFFFF;
    int count = 0, valid = 0, unreadable = -1;
    for (int i = 0; i < kMaxSongs; i++)
    {
        const uint32_t e = table + uint32_t(i) * entry_size;
        if (uint64_t(e) + entry_size > data_start)
        {
            break;
        }

        SongHeader h;
        if (!ReadSongHeader(rom, table, i, revision, h))
        {
            break;
        }

        if (IsEmptyEntry(h))
        {
            count = i + 1;
            continue;
        }

        // A header that points into the ROM, before any song's data, is most likely a song whose tracks use commands
        // that the model doesn't support.
        const int score = ScoreSong(rom, h, revision);
        if (score < h.tracks / 2)
        {
            unreadable = score >= 0 ? i : -1;
            break;
        }

        data_start = std::min(data_start, h.base);
        count = i + 1;
        valid++;
    }

    // Trailing empty entries aren't songs.
    while (count > 0)
    {
        SongHeader h;
        ReadSongHeader(rom, table, count - 1, revision, h);
        if (!IsEmptyEntry(h))
        {
            break;
        }

        count--;
    }

    return {count, valid, unreadable};
}

// Returns true if any song of the table plays notes.
bool HasNotes(const Rom& rom, uint32_t table, int count, Revision revision)
{
    for (int s = 0; s < count; s++)
    {
        SongHeader h;
        if (!ReadSongHeader(rom, table, s, revision, h) || IsEmptyEntry(h))
        {
            continue;
        }

        for (int t = 0; t < h.tracks; t++)
        {
            bool found = false;
            auto check_for_note = [&](const Command& c, uint32_t)
            {
                found |= c.op == Op::kNote || (c.op == Op::kPsgNote && (c.vol & 15));
            };
            WalkTrack(rom, h, t, revision, check_for_note, kMaxScanCommands);
            if (found)
            {
                return true;
            }
        }
    }

    return false;
}

// Walks each track of the song `h`, and fills `region_end` with the end of the data that starts at each track offset.
// Returns false if a track doesn't parse to an end.
bool FindRegionEnds(const Rom& rom, const SongHeader& h, Revision revision, std::map<uint16_t, uint32_t>& region_end)
{
    for (int t = 0; t < h.tracks; t++)
    {
        uint32_t end = 0;
        auto extend_end = [&](const Command& c, uint32_t)
        {
            end = std::max(end, c.addr + c.length);
        };
        if (!WalkTrack(rom, h, t, revision, extend_end, kMaxScanCommands))
        {
            return false;
        }

        uint32_t& e = region_end[h.offsets[t]];
        e = std::max(e, end);
    }

    return true;
}

// Checks the layout of the first `count` song table entries. Songs must follow the table, with only additional table
// entries allowed between them. Tracks must cover each song without gaps, starting at offset 0; shared track data is
// allowed. Songs must be contiguous and contain notes. Almost every byte is a valid command. Parsing alone therefore
// can't tell song data from unrelated bytes.
bool PlausibleLayout(const Rom& rom, uint32_t table, int count, Revision revision)
{
    const uint32_t entry_size = SongEntrySize(revision);
    std::vector<std::pair<uint32_t, uint32_t>> songs; // (base, end of its data)
    for (int s = 0; s < count; s++)
    {
        SongHeader h;
        if (!ReadSongHeader(rom, table, s, revision, h) || IsEmptyEntry(h))
        {
            continue;
        }

        std::map<uint16_t, uint32_t> region_end; // track offset -> end of the data starting there
        if (!FindRegionEnds(rom, h, revision, region_end))
        {
            return false;
        }

        if (region_end.begin()->first != 0)
        {
            return false;
        }

        for (auto it = region_end.begin(); std::next(it) != region_end.end(); ++it)
        {
            if (it->second != h.base + std::next(it)->first)
            {
                return false;
            }
        }

        songs.emplace_back(h.base, region_end.rbegin()->second);
    }

    if (songs.empty() || !HasNotes(rom, table, count, revision))
    {
        return false;
    }

    std::sort(songs.begin(), songs.end());
    const uint32_t gap = songs.front().first - table;
    if (songs.front().first < table || gap % entry_size || gap / entry_size < uint32_t(count))
    {
        return false;
    }

    // Between the entries counted and the songs, there can only be more of the table: empty entries, or ones that point
    // into the ROM, such as songs whose tracks don't parse. A table that starts partway through the real one has song
    // data there instead.
    for (int i = count; i < int(gap / entry_size); i++)
    {
        SongHeader h;
        if (!ReadSongHeader(rom, table, i, revision, h) || !(IsEmptyEntry(h) || HeaderInRom(rom, h)))
        {
            return false;
        }
    }

    for (size_t i = 0; i < songs.size(); i++)
    {
        size_t next = i + 1;
        while (next < songs.size() && songs[next].first == songs[i].first)
        {
            next++;
        }
        if (next < songs.size() && songs[i].second != songs[next].first)
        {
            return false;
        }
    }

    return true;
}

// Checks the songs of the first `count` song table entries, wherever they are. An edited ROM can contain a song in
// another place, with bytes that no command reaches beside its tracks and before them. A new track can leave no track
// at offset
// 0. PlausibleLayout then fails. Each track still has to parse to an end before the next one starts, and some song must
// contain notes.
bool PlausibleEditedSongs(const Rom& rom, uint32_t table, int count, Revision revision)
{
    bool any = false;
    for (int s = 0; s < count; s++)
    {
        SongHeader h;
        if (!ReadSongHeader(rom, table, s, revision, h) || IsEmptyEntry(h))
        {
            continue;
        }

        std::map<uint16_t, uint32_t> region_end; // track offset -> end of the data starting there
        if (!FindRegionEnds(rom, h, revision, region_end))
        {
            return false;
        }

        for (auto it = region_end.begin(); std::next(it) != region_end.end(); ++it)
        {
            if (it->second > h.base + std::next(it)->first)
            {
                return false;
            }
        }

        any = true;
    }

    return any && HasNotes(rom, table, count, revision);
}

// Returns the address that the code in `table` loads, or 0 if no match of the code loads one. A revision without such
// code gives an empty pattern, and no table.
uint32_t TableFromCode(const Rom& rom, const TableCode& table)
{
    return table.code.empty() ? 0 : FindLiteral(rom, table.code, table.ldr);
}

// Returns the code that loads revision `r`'s tables.
RevisionCode CodeFor(Revision r)
{
    RevisionCode c;
    if (UltimateMasters(r))
    {
        // Song start: lsls r0,r4,#3 / adds r0,r0,r4 / lsls r0,r0,#2 / ldr r1,=table / adds r2,r0,r1 / ldrh r1,[r2] /
        // ldrh r0,[r2,#2] / lsls r0,r0,#16 / orrs r1,r0. A song's entry is at table + song * 36 (song * 9 * 4).
        // Note start: ldr r3,=0xFFF / ands r3,r7 / movs r0,#0xA0 / lsls r0,#8 / ands r0,r7 / cmp r0,#0 / bne /
        // ldr r1,=sample_table. DirectSound output: ldrb r2,[r2,#3] / lsls r1,r2,#6 / ldr r0,=volume_table /
        // adds r1,r1,r0. Square 1 output: ldr r0,=freq_table / movs r7,#0 / ldrsh r1,[r5,r7] / lsls r1,#1 / adds /
        // ldrh. Noise output: ldr r1,=noise_table / movs r4,#0x24 / ldrsh r0,[r3,r4] / lsls r0,#1 / adds / ldrh.
        c.song_start = {{Exact(0x00E0), Exact(0x1900), Exact(0x0080), HiByte(0x4900), Exact(0x1842), Exact(0x8811),
                         Exact(0x8850), Exact(0x0400), Exact(0x4301)},
                        3};
        c.note_start = {{HiByte(0x4B00), Exact(0x403B), Exact(0x20A0), Exact(0x0200), Exact(0x4038), Exact(0x2800),
                         HiByte(0xD100), HiByte(0x4900)},
                        7};
        c.volume = {{Exact(0x78D2), Exact(0x0191), HiByte(0x4800), Exact(0x1809)}, 2};
        c.psg_freq = {{HiByte(0x4800), Exact(0x2700), Exact(0x5FE9), Exact(0x0049), Exact(0x1809), Exact(0x880D)}, 0};
        c.noise = {{HiByte(0x4900), Exact(0x2424), Exact(0x5F18), Exact(0x0040), Exact(0x1840), Exact(0x8800)}, 0};
        if (r == Revision::kPrinceOfTennis)
        {
            // Prince of Tennis 2004's output stage loads the output records with other registers: ldrb r2,[rX,#3] for
            // the volume, and movs rX,#0 or #0x24 / ldrsh with any registers for the frequency and noise tables.
            c.volume = {{{0x78C2, 0xFFC7}, Exact(0x0191), HiByte(0x4800), Exact(0x1809)}, 2};
            c.psg_freq = {
                {HiByte(0x4800), {0x2000, 0xF8FF}, {0x5E00, 0xFE00}, Exact(0x0049), Exact(0x1809), {0x8808, 0xFFF8}},
                0};
            c.noise = {
                {HiByte(0x4900), {0x2024, 0xF8FF}, {0x5E00, 0xFE00}, Exact(0x0040), Exact(0x1840), Exact(0x8800)}, 0};
        }

        return c;
    }

    // Dungeon Dice Monsters. Song start: lsls r0,r4,#2 / adds r0,r0,r4 / lsls r0,r0,#2 / ldr r1,=table / adds r1,r0,r1
    // / ldrh r4,[r1,#2] / lsls r0,r4,#16 / ldrh r5,[r1] / orrs r0,r5. A song's entry is at table + song * 20 (song * 5
    // * 4). Voice start: lsls r1,r2,#3 / ldr r0,=sample_table / adds r0,r1,r0 / ldr r5,[r0] / ldr r1,[r0,#4]. Square 1
    // output: ldr r1,=freq_table / movs rA,#0 / ldrsh r0,[r6,rA] / lsls r0,#1 / adds / ldrh. rA is r4 in AYDE's build
    // and r3 in BYVE's. The frequency table contains its noise settings too, and it has no volume table.
    if (r == Revision::kDungeonDiceMonsters)
    {
        c.song_start = {{Exact(0x00A0), Exact(0x1900), Exact(0x0080), HiByte(0x4900), Exact(0x1841), Exact(0x884C),
                         Exact(0x0420), Exact(0x880D), Exact(0x4328)},
                        3};
        c.song_entry = {2, false, 2};
        c.note_start = {{Exact(0x00D1), HiByte(0x4800), Exact(0x1808), Exact(0x6805), Exact(0x6841)}, 1};
        c.psg_freq = {{HiByte(0x4900), Pattern{0x2000, 0xF8FF}, Pattern{0x5E30, 0xFE3F}, Exact(0x0040), Exact(0x1840),
                       Exact(0x8800)},
                      0};
        return c;
    }

    // Eternal Duelist and Captain Tsubasa. Song start: as in Ultimate Masters, with lsls r0,r4,#1 and lsls r0,r0,#3. A
    // song's entry is at table + song * 24 (song * 3 * 8). Note start: ldr r1,=sample_table / lsls r0,r4,#2 /
    // adds r0,r0,r1 / ldr r2,[r0] / lsls r0,r3,#1 / ldr r1,=pitch_table / adds / ldrh r1,[r0] / ldr r0,[r2] /
    // muls r0,r1. Square 1 output: ldr r0,=freq_table / movs r3,#0 / ldrsh r1,[r7,r3] / lsls r1,#1 / adds /
    // ldrh r5,[r1]. Noise output: ldr r1,=noise_table / movs r3,#0x18 / ldrsh r0,[r7,r3] / lsls r0,#1 / adds / ldrh.
    // Their voices have no pan, and they have no volume table.
    if (EternalDuelistLayout(r))
    {
        c.song_start = {{Exact(0x0060), Exact(0x1900), Exact(0x00C0), HiByte(0x4900), Exact(0x1842), Exact(0x8811),
                         Exact(0x8850), Exact(0x0400), Exact(0x4301)},
                        3};
        c.song_entry = {1, false, 3};
        c.note_start = {{HiByte(0x4900), Exact(0x00A0), Exact(0x1840), Exact(0x6802), Exact(0x0058), HiByte(0x4900),
                         Exact(0x1840), Exact(0x8801), Exact(0x6810), Exact(0x4348)},
                        0};
        c.psg_freq = {{HiByte(0x4800), Exact(0x2300), Exact(0x5EF9), Exact(0x0049), Exact(0x1809), Exact(0x880D)}, 0};
        c.noise = {{HiByte(0x4900), Exact(0x2318), Exact(0x5EF8), Exact(0x0040), Exact(0x1840), Exact(0x8800)}, 0};

        // Captain Tsubasa's square 1 output has Rave Master's registers: ldr r0,=freq_table / movs r2,#0 /
        // ldrsh r1,[r7,r2] / lsls r1,#1 / adds / ldrh r5,[r1].
        if (r == Revision::kCaptainTsubasa)
        {
            c.psg_freq.code[1] = Exact(0x2200);
            c.psg_freq.code[2] = Exact(0x5EB9);
        }

        return c;
    }

    // WCT 2004. Song start: as in Ultimate Masters, with the song number in r5 and the entry's address in r3. The Rave
    // Master revision has the Ultimate Masters code with subs r0,r0,r4 in place of the adds. A Rave Master song's entry
    // is at table + song * 28 (song * 7 * 4). Note start: ldr r1,=sample_table / ldr r0,=0xFFF / ands r0,r6 /
    // lsls r0,#2 / adds r0,r0,r1 / ldr r5,[r0]. DirectSound and PSG output: ldrb r0,[r7,#3] / lsls r0,#4 /
    // ldr r1,=volume_table / adds r1,r0,r1 / movs r0,#0xF / ands r0,r4. Square 1 output: ldr r0,=freq_table /
    // movs r2,#0 / ldrsh r1,[r7,r2] / lsls r1,#1 / adds / ldrh r4,[r1]. Noise output: as in Ultimate Masters, with
    // ldrsh r0,[r7,r4]. The Rave Master revision has other registers in some of them.
    const bool rave_master = RaveMasterLayout(r);
    if (rave_master)
    {
        c.song_start = {{Exact(0x00E0), Exact(0x1B00), Exact(0x0080), HiByte(0x4900), Exact(0x1842), Exact(0x8811),
                         Exact(0x8850), Exact(0x0400), Exact(0x4301)},
                        3};
        c.song_entry.subtracts = true;
    }
    else
    {
        c.song_start = {{Exact(0x00E8), Exact(0x1940), Exact(0x0080), HiByte(0x4900), Exact(0x1843), Exact(0x8819),
                         Exact(0x8858), Exact(0x0400), Exact(0x4301)},
                        3};
    }

    c.note_start = {{HiByte(0x4900), HiByte(0x4800), Exact(rave_master ? 0x4028 : 0x4030), Exact(0x0080), Exact(0x1840),
                     Exact(rave_master ? 0x6802 : 0x6805)},
                    0};
    c.volume = {{Exact(0x78F8), Exact(0x0100), HiByte(0x4900), Exact(0x1841), Exact(0x200F),
                 Exact(rave_master ? 0x4028 : 0x4020)},
                2};
    c.psg_freq = {{HiByte(0x4800), Exact(0x2200), Exact(0x5EB9), Exact(0x0049), Exact(0x1809),
                   Exact(rave_master ? 0x880D : 0x880C)},
                  0};
    c.noise = {{HiByte(0x4900), Exact(0x2424), Exact(0x5F38), Exact(0x0040), Exact(0x1840), Exact(0x8800)}, 0};

    // The Hunter X Hunter revision's note start has the registers of WCT 2004's. Its square 1 output has
    // movs r3,#0 / ldrsh r1,[r7,r3], with ldrh r4 or r5 last. Its other outputs take the volume's low nibble from r4
    // or r5, and its noise output has movs rX,#0x24 / ldrsh r0,[r7,rX] with rX from r4 to r6.
    if (r == Revision::kHunterXHunter)
    {
        c.note_start.code[2] = Exact(0x4030);
        c.note_start.code[5] = Exact(0x6805);
        c.volume.code[5] = {0x4020, 0xFFF7};
        c.psg_freq.code[1] = Exact(0x2300);
        c.psg_freq.code[2] = Exact(0x5EF9);
        c.psg_freq.code[5] = {0x880C, 0xFFFE};
        c.noise.code[1] = {0x2024, 0xF8FF};
        c.noise.code[2] = {0x5E38, 0xFE3F};
    }

    return c;
}

// Returns the song tables that the play-song routine could load, found with the patterns in `code`. Sets `exact` if
// they come from the routine's code, rather than from code that only works out a song's entry with the same sum.
std::set<uint32_t> SongTablesFromCode(const Rom& rom, const RevisionCode& code, bool& exact)
{
    std::set<uint32_t> tables;
    for (uint32_t hit : FindThumb(rom, code.song_start.code))
    {
        tables.insert(ThumbLiteral(rom, hit + 2 * uint32_t(code.song_start.ldr)));
    }

    exact = !tables.empty();
    if (exact)
    {
        return tables;
    }

    // The same sum with any registers: a shift, an add or a subtract, and another shift.
    const SongEntrySum& sum = code.song_entry;
    const uint8_t* d = rom.Ptr(kRomBase);
    for (size_t i = 0; i + 10 <= rom.Size(); i += 2)
    {
        auto h = [&](int k)
        {
            return xsflib::LoadLe16(d + i + 2 * k);
        };

        const uint16_t a = h(0), b = h(1), c = h(2), l = h(3);
        if ((a & 0xFFC0) != uint16_t(sum.first_shift << 6)) // lsls rd, rm, #first_shift
        {
            continue;
        }

        // adds rd, rd, rm (either way round), or subs rd, rd, rm
        const int rd = a & 7, rm = (a >> 3) & 7;
        const int bd = b & 7, bn = (b >> 3) & 7, bm = (b >> 6) & 7;
        const bool adds = (b & 0xFE00) == 0x1800 && ((bn == rd && bm == rm) || (bn == rm && bm == rd));
        const bool subs = (b & 0xFE00) == 0x1A00 && bn == rd && bm == rm;
        if (bd != rd || !(sum.subtracts ? subs : adds))
        {
            continue;
        }
        if (c != uint16_t((sum.second_shift << 6) | (rd << 3) | rd)) // lsls rd, rd, #second_shift
        {
            continue;
        }
        if ((l & 0xF800) != 0x4800) // ldr rt, =table
        {
            continue;
        }

        tables.insert(ThumbLiteral(rom, kRomBase + uint32_t(i) + 6));
    }

    return tables;
}

// Scans the data for a song table: one whose first songs parse, laid out like the driver's song data. Returns the first
// such table with two or more songs, or failing that the first with one, or 0 if there's none.
uint32_t ScanForSongTable(const Rom& rom, Revision revision)
{
    const uint32_t entry_size = SongEntrySize(revision);
    uint32_t best = 0;
    int best_valid = 0;
    for (uint32_t t = kRomBase; t + 2 * entry_size <= rom.End() && best_valid < 2; t += 4)
    {
        const uint32_t base = rom.U32(t);
        if (!rom.Contains(base) || base <= t || (base - t) % entry_size)
        {
            continue;
        }

        SongHeader h0, h1;
        ReadSongHeader(rom, t, 0, revision, h0);
        ReadSongHeader(rom, t, 1, revision, h1);
        if (ScoreSong(rom, h0, revision) < h0.tracks)
        {
            continue;
        }
        if (!IsEmptyEntry(h1) && ScoreSong(rom, h1, revision) < h1.tracks)
        {
            continue;
        }

        const SongCount songs = CountSongs(rom, t, revision);
        if (songs.valid > best_valid && PlausibleLayout(rom, t, songs.entries, revision))
        {
            best = t;
            best_valid = songs.valid;
        }
    }

    return best;
}

// Finds the song table from the play-song routine's code, or failing that in the data. `code` contains the revision's
// patterns. `has_reader` says that the ROM contains the driver's command reader. Returns 0 and sets `error` if the code
// loads a table without songs, or returns 0 with `error` empty if there's no sign of a song table. `error` names
// `program`.
uint32_t FindSongTable(const Rom& rom, Revision revision, const RevisionCode& code, bool has_reader,
                       const std::string& program, std::vector<std::string>& log, std::string& error)
{
    // Choose the candidate table with the most songs. The play-song routine's code gives the table's address. Its table
    // therefore requires only one song that parses. Tables found through other code must also pass the data scan's
    // layout checks. Other revisions compute the same address, and some of their jingles can parse despite the
    // different command sets.
    bool exact = false;
    const std::set<uint32_t> candidates = SongTablesFromCode(rom, code, exact);
    uint32_t loaded = 0; // the first of them in the ROM
    uint32_t best = 0;
    int best_valid = 0;
    for (uint32_t t : candidates)
    {
        if (!rom.Contains(t))
        {
            continue;
        }

        loaded = loaded ? loaded : t;
        const SongCount songs = CountSongs(rom, t, revision);
        if (songs.valid > best_valid && (exact || PlausibleLayout(rom, t, songs.entries, revision)))
        {
            best = t;
            best_valid = songs.valid;
        }
    }

    if (best)
    {
        log.push_back("song table " + Hex(best) + " (from play-song code)");
        return best;
    }

    const char* why = candidates.empty() ? "play-song code not recognised"
                      : exact            ? "the song table that the play-song code loads isn't in the ROM"
                                         : "found code like the play-song routine, but no table it loads is laid out "
                                           "like the driver's song data";

    // An edited ROM, in which the driver's command reader shows that the game has the driver. A data scan could take a
    // part of the table whose songs stayed in their places for the table.
    if (!exact && has_reader)
    {
        for (uint32_t t : candidates)
        {
            const SongCount songs = rom.Contains(t) ? CountSongs(rom, t, revision) : SongCount{};
            if (songs.valid > best_valid && PlausibleEditedSongs(rom, t, songs.entries, revision))
            {
                best = t;
                best_valid = songs.valid;
            }
        }

        if (best)
        {
            log.push_back(why);
            log.push_back("song table " + Hex(best) + " (from that code, with songs that lie apart)");
            return best;
        }
    }

    // The play-song routine's code gives the table's address. A table found in the data instead would be the wrong
    // one.
    if (exact && loaded)
    {
        error = "found the play-song code, but the song table it loads, at " + Hex(loaded) + ", holds no songs " +
                program + " can read (--song-table and --song-count override detection)";
        return 0;
    }

    log.push_back(std::string(why) + ", scanning data for a song table");
    best = ScanForSongTable(rom, revision);
    if (best)
    {
        log.push_back("song table " + Hex(best) + " (from data scan)");
        return best;
    }

    // Nothing that contains the driver's songs: without the driver's command reader, the game has another driver.
    error.clear();

    return 0;
}

} // namespace
} // namespace agblib::konami
