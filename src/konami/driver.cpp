// SPDX-License-Identifier: MIT

#include "agblib/konami/driver.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/thumb.h"
#include "agblib/konami/seqformat.h"
#include "driver_internal.h"
#include "driver_samples.h"
#include "driver_songs.h"

namespace agblib::konami
{
namespace
{

// Returns the mixer's output rate in mixer mode 0, from the timer 0 setting in the driver's timer table, or 0 if that
// doesn't give a rate between 4000 and 65536 Hz.
double TimerRate(const Rom& rom, uint32_t timer_table)
{
    if (!timer_table)
    {
        return 0;
    }

    // The timer counts the CPU's cycles, divided by the prescaler that bits 16-17 of the setting select.
    constexpr int kPrescale[4] = {1, 64, 256, 1024};
    const uint32_t v = rom.U32(timer_table);
    const double rate = double(kSecondCycles) / ((0x10000 - (v & 0xFFFF)) * kPrescale[(v >> 16) & 3]);
    return rate >= 4000 && rate <= 65536 ? rate : 0;
}

// The older revisions of the driver that the model reads, by the fifth opcode that the command reader compares a
// command with. The fourth is EF in each of them.
constexpr std::pair<int, Revision> kOlderRevisions[] = {{0xFA, Revision::kWct2004},
                                                        {0xF9, Revision::kKitaro},
                                                        {0xFB, Revision::kMinnaNoOujisama},
                                                        {0xF5, Revision::kRaveMaster},
                                                        {0xF6, Revision::kHunterXHunter},
                                                        {0xF3, Revision::kEternalDuelist},
                                                        {0xFC, Revision::kDungeonDiceMonsters}};

// Returns the address of the later Ultimate Masters builds' duty command, or 0 if there's none. The command keeps only
// the opcode's low nibble: movs rA,#0xf / ands rB,rA / cmp rB,#3 / ble / subs rB,#4 / b / lsls rB,rB,#6.
uint32_t FindNibbleDuty(const Rom& rom)
{
    const std::vector<uint32_t> duty_code = FindThumb(rom, {{0x200F, 0xF8FF},
                                                            {0x4000, 0xFFC0},
                                                            {0x2803, 0xF8FF},
                                                            Exact(0xDD01),
                                                            {0x3804, 0xF8FF},
                                                            Exact(0xE000),
                                                            {0x0180, 0xFFC0}});
    return duty_code.empty() ? 0 : duty_code.front();
}

// Returns the later Ultimate Masters build of a driver with the duty command of FindNibbleDuty(). Live Start keeps the
// Ultimate Masters revision's code that reads a sample byte above EF with the next one: cmp rA,#0xef / ble /
// adds rB,#1 / adds rC,#1 / movs rD,#0xf / ands rD,rA / lsls rA,rD,#8. Prince of Tennis 2004 has no such code.
Revision LaterBuild(const Rom& rom)
{
    const std::vector<uint32_t> wide_samples = FindThumb(rom, {{0x28EF, 0xF8FF},
                                                               HiByte(0xDD00),
                                                               {0x3001, 0xF8FF},
                                                               {0x3001, 0xF8FF},
                                                               {0x200F, 0xF8FF},
                                                               {0x4000, 0xFFC0},
                                                               {0x0200, 0xFFC0}});
    return wide_samples.empty() ? Revision::kPrinceOfTennis : Revision::kLiveStart;
}

// Finds the driver's command reader, and works out the driver's revision from the opcodes it compares the command with:
// ldrb rX,[rY] / cmp rX,#0xFC / ble, then FF, and FD in the Ultimate Masters revision or FE in older ones. The older
// ones go on with EF and an opcode from kOlderRevisions. The later Ultimate Masters builds have a duty command that
// keeps the opcode's low nibble (see LaterBuild). Sets `reader` to the address of the reader, or to 0 if there's none.
// Returns false and sets `error` for another older revision that the model can't read. A ROM without a command reader
// that this recognises is taken to have the Ultimate Masters revision. `error` names `program`.
bool FindRevision(const Rom& rom, const std::string& program, Revision& revision, uint32_t& reader,
                  std::vector<std::string>& log, std::string& error)
{
    revision = Revision::kUltimateMasters;
    reader = 0;

    const uint8_t* d = rom.Ptr(kRomBase);
    const size_t count = rom.Size() / 2;
    auto h = [&](size_t i)
    {
        return xsflib::LoadLe16(d + 2 * i);
    };

    for (size_t i = 0; i + 3 <= count; i++)
    {
        const uint16_t load = h(i);
        if ((load & 0xFFC0) != 0x7800)
        {
            continue;
        }

        const uint16_t cmp = uint16_t(0x2800 | ((load & 7) << 8));
        if (h(i + 1) != (cmp | 0xFC) || (h(i + 2) & 0xFF00) != 0xDD00)
        {
            continue;
        }

        // The first nine opcodes that the next 200 instructions compare the register with. Comparisons with 0 check a
        // command's argument.
        std::vector<int> opcodes;
        for (size_t k = i + 1; k < std::min(count, i + 200) && opcodes.size() < 9; k++)
        {
            if ((h(k) & 0xFF00) == cmp && (h(k) & 0xFF) != 0)
            {
                opcodes.push_back(h(k) & 0xFF);
            }
        }

        if (opcodes.size() < 5 || opcodes[1] != 0xFF || (opcodes[2] != 0xFD && opcodes[2] != 0xFE))
        {
            continue;
        }

        reader = kRomBase + uint32_t(2 * i);
        const auto* older = std::find_if(std::begin(kOlderRevisions), std::end(kOlderRevisions),
                                         [&](const auto& r) { return r.first == opcodes[4]; });
        if (opcodes[2] == 0xFE && (opcodes[3] != 0xEF || older == std::end(kOlderRevisions)))
        {
            error = "this game has an older revision of Konami's driver with commands that " + program +
                    " can't read (its command reader is at " + Hex(reader) + ")";
            return false;
        }

        revision = opcodes[2] == 0xFD ? Revision::kUltimateMasters : older->second;

        // The Captain Tsubasa revision's reader compares the opcodes of Rave Master's, with F0 in place of F1.
        if (revision == Revision::kRaveMaster && opcodes.size() == 9 && opcodes[8] == 0xF0)
        {
            revision = Revision::kCaptainTsubasa;
        }

        const uint32_t duty = revision == Revision::kUltimateMasters ? FindNibbleDuty(rom) : 0;
        if (duty)
        {
            revision = LaterBuild(rom);
        }

        log.push_back(std::string(RevisionName(revision)) + " revision of the driver (from the command reader at " +
                      Hex(reader) + (duty ? " and the duty command at " + Hex(duty) : std::string()) + ")");
        return true;
    }

    if (const uint32_t duty = FindNibbleDuty(rom))
    {
        revision = LaterBuild(rom);
        log.push_back("command reader not recognised, " + std::string(RevisionName(revision)) +
                      " revision of the driver (from the duty command at " + Hex(duty) + ")");
        return true;
    }

    log.push_back("command reader not recognised, assuming the Ultimate Masters revision of the driver");

    return true;
}

// Finds the Dungeon Dice Monsters revision's tables from the code that loads them. Returns false and sets `error` if
// the sample map or the sample period table isn't there. The revision's notes read both.
bool FindDungeonDiceTables(const Rom& rom, DriverInfo& info, std::string& error)
{
    // Sample note: ldr rA,=sample_map / ldrb rB,[rC,#1] / adds rD,rE,rF / ldrb r0,[r0] / strb r0,[rG,#2]. Sample bend:
    // ldr r0,=period_table / lsls r1,rA,#1 / adds r1,r1,r0 / ldrh r0,[r1] / strh r0,[rB] / ldrh r0,[r1] /
    // strh r0,[rC]. The builds give the other registers different numbers.
    constexpr Pattern kAnyLdr{0x4800, 0xF800};
    info.sample_map = FindLiteral(
        rom, {kAnyLdr, Pattern{0x7840, 0xFFC0}, Pattern{0x1800, 0xFE00}, Exact(0x7800), Pattern{0x7080, 0xFFC7}}, 0);
    info.sample_period_table = FindLiteral(rom,
                                           {HiByte(0x4800), Pattern{0x0041, 0xFFC7}, Exact(0x1809), Exact(0x8808),
                                            Pattern{0x8000, 0xFFC7}, Exact(0x8808), Pattern{0x8000, 0xFFC7}},
                                           0);
    if (!info.sample_map || !info.sample_period_table)
    {
        error =
            "found the command reader of the Dungeon Dice Monsters revision, but not the sample map or the sample "
            "period table that its notes read";
        return false;
    }

    // Wave command: lsls r0,rA,#4 / ldr r1,=wave_table / adds r0,r0,r1 / str r0,[r2]. Wave output:
    // ldr r1,=volume_table / ldrb rA,[r6,#0x1b] / lsls r0,rA,#1 / adds r0,r0,r1.
    info.wave_table = FindLiteral(rom, {Pattern{0x0100, 0xFFC7}, HiByte(0x4900), Exact(0x1840), Exact(0x6010)}, 1);
    info.wave_volume_table =
        FindLiteral(rom, {HiByte(0x4900), Pattern{0x7EF0, 0xFFF8}, Pattern{0x0040, 0xFFC7}, Exact(0x1840)}, 0);

    // Vibrato: asrs rA,rA,#2 / ldr r0,=vibrato_part / ldr r1,=freq_table / subs r0,r0,r1. The vibrato's part of the
    // frequency table starts at the entry the difference gives.
    const std::vector<uint32_t> vibrato =
        FindThumb(rom, {Pattern{0x1080, 0xFFC0}, HiByte(0x4800), HiByte(0x4900), Exact(0x1A40)});
    if (vibrato.empty())
    {
        info.warnings.push_back("vibrato code not recognised, so vibrato plays at the wrong pitches");
    }
    else
    {
        const uint32_t part = ThumbLiteral(rom, vibrato.front() + 2);
        const uint32_t table = ThumbLiteral(rom, vibrato.front() + 4);
        info.vibrato_entry = int32_t(part - table) / 2;
    }

    return true;
}

// Checks that the table addresses in `overrides` fall within the ROM. Returns false and sets `error` if one doesn't.
bool CheckOverrides(const Rom& rom, const DriverOverrides& overrides, std::string& error)
{
    for (const auto& [what, address] :
         {std::pair("song", overrides.song_table), std::pair("sample", overrides.sample_table)})
    {
        if (address && !rom.Contains(address))
        {
            error = std::string("the ") + what + " table address " + Hex(address) + " is outside the ROM (" +
                    Hex(kRomBase) + "-" + Hex(rom.End() - 1) + ")";
            return false;
        }
    }

    return true;
}

// Finds the song table and counts its songs, unless `overrides` gives them. `reader` is the address of the command
// reader, or 0 if there's none. Returns false and sets `error` if there's no song table with songs the model can
// read.
bool FindSongs(const Rom& rom, const DriverOverrides& overrides, const RevisionCode& code, uint32_t reader,
               DriverInfo& info, std::string& error)
{
    if (overrides.song_table)
    {
        info.song_table = overrides.song_table;
        info.log.push_back("song table " + Hex(overrides.song_table) + " (user supplied)");
    }
    else
    {
        info.song_table = FindSongTable(rom, info.revision, code, reader != 0, overrides.program, info.log, error);
        if (!info.song_table)
        {
            // The command reader shows that the game has the driver. A song table that can't be found is an error.
            if (reader && error.empty())
            {
                error = "found the command reader of the " + std::string(RevisionName(info.revision)) +
                        " revision of Konami's driver, at " + Hex(reader) +
                        ", but not its song table (--song-table and --song-count override detection)";
            }

            return false;
        }
    }

    if (overrides.song_count > 0)
    {
        info.song_count = overrides.song_count;
    }
    else
    {
        const SongCount songs = CountSongs(rom, info.song_table, info.revision);
        info.song_count = songs.entries;
        if (songs.unreadable >= 0)
        {
            info.warnings.push_back("song " + std::to_string(songs.unreadable) +
                                    "'s tracks don't read cleanly, so the song table is taken to end before it "
                                    "(--song-count overrides this)");
        }
    }

    if (info.song_count <= 0)
    {
        error = "the song table at " + Hex(info.song_table) + " holds no songs " + overrides.program +
                " can read (--song-count overrides this)";
        return false;
    }

    return true;
}

// Finds the sample table, unless `overrides` gives it. Returns false and sets `error` if the songs play samples and
// there's no table that the model can use.
bool FindSamples(const Rom& rom, const DriverOverrides& overrides, const RevisionCode& code, DriverInfo& info,
                 std::string& error)
{
    if (overrides.sample_table)
    {
        info.sample_table = overrides.sample_table;
        info.log.push_back("sample table " + Hex(overrides.sample_table) + " (user supplied)");
        return true;
    }

    const std::vector<int> used = UsedSamples(rom, info.song_table, info.song_count, info.revision, info.sample_map);
    info.sample_table =
        FindSampleTable(rom, used, code.note_start, info.revision, overrides.program, info.log, info.warnings, error);
    return info.sample_table != 0 || used.empty();
}

// Finishes the detection of the Dungeon Dice Monsters revision, and logs its tables. The revision's noise settings are
// entries of the frequency table. The revision has no mixer: each FIFO plays at the rate of the samples on it.
void FinishDungeonDice(DriverInfo& info)
{
    constexpr uint32_t kNoiseEntry = 16 * kDungeonDiceNoiseNote;
    info.noise_table = info.psg_freq_table ? info.psg_freq_table + 2 * kNoiseEntry : 0;
    if (!info.psg_freq_table)
    {
        info.warnings.push_back(
            "PSG frequency table not found, so square notes are left out, wave notes play at "
            "the wrong pitch and noise notes use default settings");
    }

    char buf[160];
    std::snprintf(buf, sizeof buf, "%d song%s, each sample at its rate, PSG freq table %s, noise table %s",
                  info.song_count, info.song_count == 1 ? "" : "s",
                  info.psg_freq_table ? Hex(info.psg_freq_table).c_str() : "(not found)",
                  info.noise_table ? Hex(info.noise_table).c_str() : "(default)");
    info.log.push_back(buf);

    std::snprintf(buf, sizeof buf, "sample map %s, sample period table %s, wave table %s, wave volume table %s",
                  Hex(info.sample_map).c_str(), Hex(info.sample_period_table).c_str(),
                  info.wave_table ? Hex(info.wave_table).c_str() : "(default)",
                  info.wave_volume_table ? Hex(info.wave_volume_table).c_str() : "(default)");
    info.log.push_back(buf);

    info.bend_kept = false;
}

// Finds the wave tables from the code that loads a wave into the wave RAM.
void FindWaveTables(const Rom& rom, DriverInfo& info)
{
    // Wave RAM loader: push {r4,lr} / adds r4,r0,#0 / ldr r3,=wave_table / cmp r2,#0 / beq / movs r0,#0x80 / ands r0,r1
    // / cmp r0,#0 / beq / ldr r3,=sfx_wave_table. A wave from 80 up comes from the second table, the table of the sound
    // effects. A GSF rip zeroes the code of the second table: the songs don't run it.
    const std::vector<uint32_t> wave_code = FindThumb(rom, {Exact(0xB510), Exact(0x1C04), HiByte(0x4B00), Exact(0x2A00),
                                                            HiByte(0xD000), Exact(0x2080), Exact(0x4008)});
    for (uint32_t hit : wave_code)
    {
        if (!rom.Contains(ThumbLiteral(rom, hit + 4)))
        {
            continue;
        }

        info.wave_table = ThumbLiteral(rom, hit + 4);
        const bool second = rom.U16(hit + 14) == 0x2800 && (rom.U16(hit + 16) & 0xFF00) == 0xD000 &&
                            (rom.U16(hit + 18) & 0xFF00) == 0x4B00 && rom.Contains(ThumbLiteral(rom, hit + 18));
        info.sfx_wave_table = second ? ThumbLiteral(rom, hit + 18) : 0;
        break;
    }

    // Some builds of the Rave Master revision have no check for volume 0. Their loader goes straight from the first
    // table's load to the test for waves from 80 up: push {r4,lr} / adds r4,r0,#0 / ldr r3,=wave_table / movs r0,#0x80
    // / ands r0,r1 / cmp r0,#0 / beq / ldr r3,=sfx_wave_table.
    if (!info.wave_table)
    {
        for (uint32_t hit : FindThumb(rom, {Exact(0xB510), Exact(0x1C04), HiByte(0x4B00), Exact(0x2080), Exact(0x4008),
                                            Exact(0x2800), HiByte(0xD000), HiByte(0x4B00)}))
        {
            if (rom.Contains(ThumbLiteral(rom, hit + 4)))
            {
                info.wave_table = ThumbLiteral(rom, hit + 4);
                info.sfx_wave_table = rom.Contains(ThumbLiteral(rom, hit + 14)) ? ThumbLiteral(rom, hit + 14) : 0;
                break;
            }
        }
    }

    // The Eternal Duelist revision's loader has no check for volume 0 and no second table: push {r4,lr} /
    // lsls r1,r1,#4 / adds r1,r1,r2 / lsls r1,r1,#4 / ldr r2,=wave_table / adds r1,r1,r2.
    if (!info.wave_table)
    {
        info.wave_table = FindLiteral(
            rom, {Exact(0xB510), Exact(0x0109), Exact(0x1889), Exact(0x0109), HiByte(0x4A00), Exact(0x1889)}, 4);
    }
}

// Returns the address of the driver's timer table, or of a timer setting that stands for the table, or 0 if there's
// none.
uint32_t FindTimerTable(const Rom& rom)
{
    // Mixer rate: ldr r1,=timer_table / lsls r0,r3,#2 / adds / ldr r0,[r0] / movs r1,#0x80 / lsls r1,#16 / orrs / str.
    // Some builds load the setting into r3 instead: ldr r3,[r0] / movs r0,#0x80 / lsls r0,#16 / orrs r0,r3.
    uint32_t table = FindLiteral(rom,
                                 {HiByte(0x4900), Exact(0x0098), Exact(0x1840), Exact(0x6800), Exact(0x2180),
                                  Exact(0x0409), Exact(0x4308), Exact(0x6010)},
                                 0);
    if (!table)
    {
        table = FindLiteral(rom,
                            {HiByte(0x4900), Exact(0x0098), Exact(0x1840), Exact(0x6803), Exact(0x2080), Exact(0x0400),
                             Exact(0x4318), Exact(0x6010)},
                            0);
    }

    // The Rave Master revision loads the setting itself: ldr r0,[r1,#8] / adds r1,#0x38 / ldr r0,=setting /
    // str r0,[r1]. Eternal Duelist has ldr r1,=0x04000100 / ldr r0,=setting / str r0,[r1]. The literal then stands for
    // the table.
    if (!table)
    {
        const std::vector<uint32_t> hits =
            FindThumb(rom, {Exact(0x6888), Exact(0x3138), HiByte(0x4800), Exact(0x6008)});
        table = hits.empty() ? 0 : ThumbLiteralAddress(rom, hits.front() + 4);
    }

    if (!table)
    {
        for (uint32_t hit : FindThumb(rom, {HiByte(0x4900), HiByte(0x4800), Exact(0x6008)}))
        {
            if (ThumbLiteral(rom, hit) == 0x04000100)
            {
                table = ThumbLiteralAddress(rom, hit + 2);
                break;
            }
        }
    }

    return table;
}

// Sets the mixer's output rate: the rate in `overrides`, or else the rate that the timer table gives, or else 21024 Hz.
void SetMixRate(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info)
{
    const double timer_rate = TimerRate(rom, info.timer_table);
    if (overrides.mix_rate > 0)
    {
        info.mix_rate = overrides.mix_rate;
    }
    else if (timer_rate > 0)
    {
        info.mix_rate = timer_rate;
    }
    else
    {
        info.mix_rate = double(kSecondCycles) / (0x10000 - 0xFCE2);
        info.warnings.push_back(info.timer_table
                                    ? "mixer rate in the timer table is outside 4000-65536 Hz, assuming 21024 Hz"
                                    : "mixer timer not found, assuming 21024 Hz");
    }
}

// Logs the song count, the mixer rate and the tables, for every revision but Dungeon Dice Monsters.
void LogTables(DriverInfo& info)
{
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d song%s, mixer %.2f Hz, volume table %s, PSG freq table %s", info.song_count,
                  info.song_count == 1 ? "" : "s", info.mix_rate,
                  info.volume_table ? Hex(info.volume_table).c_str() : "(computed)",
                  info.psg_freq_table ? Hex(info.psg_freq_table).c_str() : "(default)");
    info.log.push_back(buf);

    std::snprintf(buf, sizeof buf, "noise table %s, wave table %s, timer table %s",
                  info.noise_table ? Hex(info.noise_table).c_str() : "(default)",
                  info.wave_table ? Hex(info.wave_table).c_str() : "(default)",
                  info.timer_table ? Hex(info.timer_table).c_str() : "(default)");
    info.log.push_back(buf);
}

// Works out whether the driver keeps a pitch bend for the notes after it. In the Ultimate Masters revision, the answer
// comes from the bend command's code and goes into the log.
void FindBendKept(const Rom& rom, DriverInfo& info)
{
    // The other revisions, and the later Ultimate Masters builds, never keep a bend.
    if (info.revision != Revision::kUltimateMasters)
    {
        info.bend_kept = false;
        return;
    }

    // Pitch bend: the bend command hands on the bent pitch with ldrh r0,[r6,#0x1c] / mov r7,r8 / strh r0,[r7,#6] /
    // ldrh r0,[r6] / adds r0,r0,r4 / strh r0,[r7] / ldrb r0,[r7,#5] / movs r1,#0x20 / orrs r0,r1 / strb r0,[r7,#5]. A
    // driver that keeps the bend for later notes stores it next, with strh r4,[r6,#0x1e].
    const std::vector<uint32_t> bend_code =
        FindThumb(rom, {Exact(0x8BB0), Exact(0x4647), Exact(0x80F8), Exact(0x8830), Exact(0x1900), Exact(0x8038),
                        Exact(0x7978), Exact(0x2120), Exact(0x4308), Exact(0x7178)});
    if (bend_code.empty())
    {
        info.log.push_back("pitch bend code not recognised, assuming a bend carries over to later notes");
    }
    else
    {
        info.bend_kept = rom.U16(bend_code.front() + 20) == 0x83F4;
        info.log.push_back(info.bend_kept ? "pitch bends carry over to later notes (from bend code)"
                                          : "pitch bends apply to the playing note only (from bend code)");
    }
}

// A game whose live stage sets FA's setting to 0 after it requests a song, and gives F6 a table of samples for some
// songs. Detection can't find them: the songs and the tables come from tables in the game's code.
struct LiveStage
{
    const char* game_code;
    Revision revision;
    uint32_t stage_songs;             // the song of each of the live stage's entries, a byte or a halfword each
    uint32_t song_size;               // 1 or 2
    int stages;                       // the live stage's entries
    uint32_t stage_samples;           // for each entry, a pointer to its list of sample numbers; FFFF ends a list
    uint16_t sample_flags;            // the bits that the request sets in each of the list's sample numbers
    uint32_t sample_table;            // the second sample table (DriverInfo::live_sample_table)
    std::initializer_list<int> songs; // the songs whose FA the live stage plays
};

// The values come from the games' code.
const LiveStage kLiveStages[] = {
    // Mermaid Melody Pichi Pichi Pitch: the live stage's request at 0x0800C254 plays the songs of the table at
    // 0x08EB7E08. It copies the entry's list to a table in RAM after an entry of 0, with bits 14 and 15 set, and gives
    // the driver that table through 0x080158EC. It then sets FA's setting to 0 through 0x080166AC. Each of the nine
    // songs has FA 01 on track 12. The song select's previews of songs 0 to 7 at 0x0800E66E play them in the state that
    // a request leaves. The note start at 0x08015F48 reads the second sample table.
    {"BMAJ", Revision::kWct2004, 0x08EB7E08, 2, 9, 0x08EB10D4, 0xC000, 0x0833D750, {0, 1, 2, 3, 4, 5, 6, 7, 8}},
    // Mermaid Melody Pichi Pichitto Live Start!: the live stage's request at 0x0801E1C8 plays the songs of the table at
    // 0x09D37848. It copies the entry's list as BMAJ's does, with bits 12, 14 and 15 set, and gives the driver the
    // table through 0x0800B70C. It then sets FA's setting to 0 through 0x0800C514. So does 0x080106CC for songs 17 to
    // 20. FA 01 contains track 12 or 14 of songs 2, 4 and 17 to 20. 0x0800FBA2 plays songs 17 to 20 as a request leaves
    // them. Songs 14 and 15 have FA 01 too, but the live stage plays neither of them. The note start at 0x0800BD94
    // reads the second sample table.
    {"B3MJ", Revision::kLiveStart, 0x09D37848, 1, 18, 0x09D37800, 0xD000, 0x08772980, {2, 4, 17, 18, 19, 20}},
};

// Returns `songs` as a list, such as "0, 1, 2".
std::string SongList(const std::vector<int>& songs)
{
    std::string list;
    for (int song : songs)
    {
        list += (list.empty() ? "" : ", ") + std::to_string(song);
    }

    return list;
}

// Sets the songs that sound different in the game's live stage, and the tables of samples that it gives F6, if the
// game has a live stage.
void FindLiveSongs(const Rom& rom, DriverInfo& info)
{
    const std::string code = rom.GameCode();
    for (const LiveStage& stage : kLiveStages)
    {
        if (code != stage.game_code || info.revision != stage.revision)
        {
            continue;
        }

        // The request copies the list into a table after an entry of 0. A song that two entries play takes the first
        // entry's list.
        for (int i = 0; i < stage.stages; i++)
        {
            const uint32_t at = stage.stage_songs + uint32_t(i) * stage.song_size;
            const int song = stage.song_size == 1 ? rom.U8(at) : rom.U16(at);
            std::vector<uint16_t> table{0};
            for (uint32_t p = rom.U32(stage.stage_samples + 4 * uint32_t(i));
                 rom.Contains(p, 2) && table.size() <= 0x63; p += 2)
            {
                if (rom.U16(p) == 0xFFFF)
                {
                    break;
                }

                table.push_back(uint16_t(rom.U16(p) | stage.sample_flags));
            }
            if (table.size() > 1 && song < info.song_count && !info.live_tables.count(song))
            {
                info.live_tables[song] = std::move(table);
            }
        }

        std::vector<int> heard;
        for (int song : stage.songs)
        {
            if (song < info.song_count)
            {
                heard.push_back(song);
            }
        }

        std::vector<int> tabled;
        for (const auto& entry : info.live_tables)
        {
            tabled.push_back(entry.first);
        }

        std::set_union(heard.begin(), heard.end(), tabled.begin(), tabled.end(), std::back_inserter(info.live_songs));
        info.live_sample_table = stage.sample_table;
        if (!heard.empty())
        {
            info.log.push_back("the live stage plays songs " + SongList(heard) + " with FA's setting at 0 (game " +
                               code + ")");
        }

        if (!tabled.empty())
        {
            info.log.push_back("the live stage gives F6 a table of samples for songs " + SongList(tabled) +
                               ", from the second sample table " + Hex(stage.sample_table) + " (game " + code + ")");
        }
    }
}

} // namespace

std::optional<uint16_t> TableEntry(const Rom& rom, uint32_t table, int index)
{
    const uint32_t a = table + 2 * uint32_t(index);
    if (!table || !rom.Contains(a, 2))
    {
        return std::nullopt;
    }

    return rom.U16(a);
}

int DriverInfo::TableLevel(const Rom& rom, int volume, int pan) const
{
    // In the older revisions, a row of the volume table has 16 pan levels.
    const int pan_levels = UltimateMasters(revision) ? 64 : 16;
    volume = std::clamp(volume, 0, 255);
    pan = std::clamp(pan, 0, pan_levels - 1);

    if (volume_table)
    {
        const uint32_t a = volume_table + uint32_t(volume * pan_levels + pan);
        if (rom.Contains(a))
        {
            return rom.U8(a);
        }
    }

    return volume * pan / (pan_levels - 1);
}

int DriverInfo::VoiceLevel(const Rom& rom, int volume, int pan) const
{
    // The voices of the Eternal Duelist, Captain Tsubasa and Dungeon Dice Monsters revisions have no pan. The mixer of
    // the first two plays a voice at (volume + 1)/16 on both sides, and not at all at volume 0. Dungeon Dice Monsters
    // mixes two voices into each FIFO at (volume + 1)/32 each, and plays both FIFOs on both sides.
    if (EternalDuelistLayout(revision) || revision == Revision::kDungeonDiceMonsters)
    {
        volume = std::clamp(volume, 0, 15);
        return volume ? (volume + 1) * (EternalDuelistLayout(revision) ? 8 : 4) : 0;
    }

    // The mixer of the WCT 2004, Kitaro and Minna no Ouji-sama revisions plays a level of n at amplitude n/16, and the
    // mixer of the Rave Master and Hunter X Hunter revisions at n/15.
    const int level = TableLevel(rom, volume, pan);
    switch (revision)
    {
    case Revision::kWct2004:
    case Revision::kKitaro:
    case Revision::kMinnaNoOujisama:
        return level * 8;
    case Revision::kRaveMaster:
    case Revision::kHunterXHunter:
        return (level * 128 + 7) / 15;
    default:
        return level;
    }
}

uint16_t DriverInfo::NoiseSetting(const Rom& rom, int note) const
{
    // The driver reads whatever is at the note's place, before the table for a negative note.
    if (const std::optional<uint16_t> setting = TableEntry(rom, noise_table, note))
    {
        return *setting;
    }

    // Fallback matching Yu-Gi-Oh! Ultimate Masters Edition's table: shift = note + 1.
    return uint16_t(0x8000 | ((std::clamp(note, 0, 12) + 1) << 4));
}

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    if (!CheckOverrides(rom, overrides, error))
    {
        return false;
    }

    uint32_t reader = 0;
    if (!FindRevision(rom, overrides.program, info.revision, reader, info.log, error))
    {
        return false;
    }

    const RevisionCode code = CodeFor(info.revision);
    const bool dungeon_dice = info.revision == Revision::kDungeonDiceMonsters;
    if (dungeon_dice && !FindDungeonDiceTables(rom, info, error))
    {
        return false;
    }
    if (!FindSongs(rom, overrides, code, reader, info, error) || !FindSamples(rom, overrides, code, info, error))
    {
        return false;
    }

    info.volume_table = TableFromCode(rom, code.volume);
    info.psg_freq_table = TableFromCode(rom, code.psg_freq);
    info.noise_table = TableFromCode(rom, code.noise);
    if (dungeon_dice)
    {
        FinishDungeonDice(info);
        return true;
    }

    FindWaveTables(rom, info);
    info.timer_table = FindTimerTable(rom);
    SetMixRate(rom, overrides, info);
    LogTables(info);
    FindBendKept(rom, info);
    FindLiveSongs(rom, info);

    return true;
}

std::vector<int8_t> SamplePcm(const Rom& rom, const SampleInfo& s)
{
    if (!s.valid || s.length <= 0)
    {
        return {};
    }
    if (s.packed)
    {
        return UnpackSample(rom, s);
    }

    std::vector<int8_t> pcm(size_t(s.length));
    for (size_t i = 0; i < pcm.size(); i++)
    {
        pcm[i] = rom.S8(s.data + uint32_t(i));
    }

    return pcm;
}

SampleInfo DriverInfo::Sample(const Rom& rom, int index) const
{
    // The second sample table contains packed samples in both games that have one. BMAJ's note start reads them as Live
    // Start's does.
    if (index >= 0x8000 && live_sample_table)
    {
        const SampleInfo s = PackedSampleAt(rom, live_sample_table, index & 0xFFF);
        return s.valid ? s : SampleHeaderAt(rom, live_sample_table, index & 0xFFF);
    }

    if (!sample_table || index < 0)
    {
        return SampleInfo();
    }

    return SampleAt(rom, sample_table, index, revision);
}

} // namespace agblib::konami
