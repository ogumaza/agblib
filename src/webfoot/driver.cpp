// SPDX-License-Identifier: MIT

#include "agblib/webfoot/driver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/thumb.h"

namespace agblib::webfoot
{

using agblib::agb::Hex;
using agblib::agb::kRomBase;

namespace
{

// The CPU's clock, in cycles a second.
constexpr double kCpuClock = 16777216.0;

// The 2002 build sets its timer and its steps from a rate that the game passes to its init routine (0x0800A9F6 in
// Dragon Ball Z: The Legacy of Goku): the step factor is 2^26 over the rate, rounded up (0x0800AA0A), and the timer
// counts (factor + 3) / 4, rounded up (0x0800AAB2 to 0x0800AAC2). A tick lasts 2.5 times the rate over the tempo, in
// samples. The Legacy of Goku passes 14,000 Hz: a factor of 4,794 and a timer of 1,200 cycles.
constexpr uint32_t k2002Rate = 14000;
constexpr uint32_t k2002StepFactor = 4794;
constexpr uint32_t k2002TimerCycles = 1200;
constexpr uint32_t k2002TickScale = 35000;

// The later build sets its timer's reload from a literal in the init, to 1,050 cycles in every program, and works the
// steps and the ticks out for a rate of 16,000 Hz.
constexpr uint32_t k2003StepFactor = 0x1063;
constexpr uint32_t k2003TimerCycles = 1050;
constexpr uint32_t k2003TickScale = 40000;

// The code of the setter of the music volume and of the 2002 build's init: 0x0800A9CC and 0x0800A9F6 in The Legacy of
// Goku, 0x08020AC8 in The Legacy of Goku II.
constexpr std::string_view k2002Volume = "0602 0E12 B430 48xx 4Cxx 3818 7002";
constexpr std::string_view k2002Init = "B5F8 4Cxx 2300 3C18 60A3 0400 0C00 60E3";
constexpr std::string_view k2003Volume = "B430 2410 2200 0609 0E09 0053";

// The later build's init passes the music volume of 4 to the setter: `movs r1, #4`, one or two instructions before the
// call. It starts with a push of lr, then `adds r4, r0, #0`.
constexpr uint16_t kMovsR1Volume = 0x2104;
constexpr uint16_t kPushLr = 0xB500;
constexpr uint16_t kAddsR4R0 = 0x1C04;

// The later build's init stores timer 0's reload from a literal: `ldr rM, =0xFFFFnnnn`, then within three
// instructions `strh rM, [rN, #0]` (0x08020B72 in The Legacy of Goku II).
constexpr uint32_t kReloadStoreReach = 3;

// The most samples that the mix routine mixes at a time: 256 in the 2002 build (0x0800ABE4 in The Legacy of Goku) and
// 512 in the later one (0x08020C08 in The Legacy of Goku II). The routine that a ping-pong loop turns in depends on
// where a block starts. The model therefore mixes the same blocks.
constexpr uint32_t k2002Block = 256;
constexpr uint32_t k2003Block = 512;

// The first entries of the fine slides up and down. The tables of slides follow them, 64 halfwords on. Each copy of the
// driver contains the steps, the slides up, the slides down and the vibrato's sine in that order.
constexpr std::array<uint16_t, 4> kFineUpStart = {32768, 32798, 32827, 32857};
constexpr std::array<uint16_t, 4> kFineDownStart = {65535, 65477, 65418, 65359};

// The most bytes between the end of the steps and the slides up: 0 in the 2002 build, 128 in the later one.
constexpr uint32_t kMaxStepsGap = 0x100;

// The first quarter of the vibrato's sine.
constexpr std::array<int8_t, 16> kVibratoStart = {0, 6, 12, 19, 24, 30, 36, 41, 45, 49, 53, 56, 59, 61, 63, 64};

// The start of a program's crt0: `mov r0, #0x92; msr cpsr_fc, r0`, at 0xC0 into the program's header.
constexpr std::array<uint8_t, 8> kCrt0Start = {0x92, 0x00, 0xA0, 0xE3, 0x00, 0xF0, 0x21, 0xE1};
constexpr uint32_t kCrt0Offset = 0xC0;

// The words of a crt0 that give the image of IWRAM: its source, its destination and its size.
constexpr uint32_t kCrt0ImageSource = kCrt0Offset + 0x74 + 5 * 4;
constexpr uint32_t kCrt0ImageDestination = kCrt0Offset + 0x74 + 6 * 4;
constexpr uint32_t kCrt0ImageSize = kCrt0Offset + 0x74 + 7 * 4;
constexpr uint32_t kIwram = 0x03000000;

// The samples of a ring of the FIFOs. The handler runs the mix routine when the mixer's lead over the FIFOs falls to
// its mark, and the mix fills the ring up to the FIFOs' place: the ring less the mark.
constexpr uint32_t kRingSamples = 1024;

// The handler's test of the mark. The 2002 build's is Thumb: `cmp r1, #mark; beq; bx r6` (0x08008ADA in The Legacy of
// Goku). The later build's is ARM, in the image of IWRAM: `cmp ip, #mark << 22; bxne r4` (0x030007F0 in The Legacy of
// Goku II).
constexpr uint16_t k2002MarkCompare = 0x2900;
constexpr uint16_t k2002ReturnR6 = 0x4730;
constexpr uint32_t k2003MarkCompare = 0xE35C0500;
constexpr uint32_t k2003ReturnR4 = 0x112FFF14;

// The code of the 2002 build that reads a table: the bytes before and after a `ldr rN, [pc, #imm]`. The note routine
// works out an instrument's entry in the sample table, and the row reader looks up a mode's mask.
struct Signature
{
    std::vector<uint8_t> before;
    uint8_t load = 0; // the high byte of the load
    std::vector<uint8_t> after;
};

// lsls r3, r4, #1; adds r3, r3, r4; ldr r4, [pc, #imm]; lsls r3, r3, #2
const Signature kSampleTableLoad = {{0x63, 0x00, 0x1B, 0x19}, 0x4C, {0x9B, 0x00}};
// ldr r5, [pc, #imm]; ldr r2, [r1, #16]; ldrb r0, [r2]; adds r2, #1
const Signature kMaskTableLoad = {{}, 0x4D, {0x0A, 0x69, 0x10, 0x78, 0x01, 0x32}};

// The music's volume that a game passes to the driver from its options at its start, where the release has a row. The
// games' code passes the options' value through a variable. Detection therefore can't read it.
struct GameVolume
{
    const char* game_code;
    uint8_t version;
    int program; // the program's place among the cartridge's programs, from 0
    int music_volume;
};

constexpr GameVolume kGameVolumes[] = {
    // Dragon Ball Z: The Legacy of Goku sets its options' music volume to 4 at 0x08000738, and passes it to the driver
    // at 0x0800028E.
    {"ALGE", 0, 0, 4},
    // The Legacy of Goku II sets 4 at 0x08001E98 when the save has no valid options, and passes it at 0x08001EAC.
    {"ALFE", 0, 0, 4},
    // Buu's Fury passes its options' music volume, 4 by default, at 0x08006B4A.
    {"BG3E", 0, 0, 4},
    // The cartridge of Buu's Fury and GT Transformation: the menu has only the init's volume, Buu's Fury passes its
    // options' volume, 4 by default, at 0x08036B4A, and GT Transformation sets its options' volume to 3 at 0x08804690
    // when the save has no valid options, and passes it at 0x08804744.
    {"BUFE", 0, 0, 4},
    {"BUFE", 0, 1, 4},
    {"BUFE", 0, 2, 3},
};

// Returns the ROM's bytes.
std::span<const uint8_t> Bytes(const Rom& rom)
{
    return {rom.Ptr(kRomBase), rom.Size()};
}

// Returns the address of each place in the ROM that contains `pattern` at a multiple of `align`.
std::vector<uint32_t> FindAll(const Rom& rom, std::span<const uint8_t> pattern, uint32_t align)
{
    std::vector<uint32_t> found;
    const std::span<const uint8_t> bytes = Bytes(rom);
    if (pattern.empty() || bytes.size() < pattern.size())
    {
        return found;
    }

    auto it = bytes.begin();
    while (true)
    {
        it = std::search(it, bytes.end(), pattern.begin(), pattern.end());
        if (it == bytes.end())
        {
            break;
        }

        const uint32_t offset = uint32_t(it - bytes.begin());
        if (offset % align == 0)
        {
            found.push_back(kRomBase + offset);
        }

        ++it;
    }

    return found;
}

template <typename T, size_t N>
std::vector<uint8_t> LittleEndian(const std::array<T, N>& values)
{
    std::vector<uint8_t> bytes;
    for (const T v : values)
    {
        for (size_t i = 0; i < sizeof(T); i++)
        {
            bytes.push_back(uint8_t(uint64_t(v) >> (8 * i)));
        }
    }

    return bytes;
}

// Whether the ROM contains `values` at `address`.
template <typename T, size_t N>
bool Holds(const Rom& rom, uint32_t address, const std::array<T, N>& values)
{
    const std::vector<uint8_t> bytes = LittleEndian(values);
    if (!rom.Contains(address, uint32_t(bytes.size())))
    {
        return false;
    }

    return std::equal(bytes.begin(), bytes.end(), rom.Ptr(address));
}

// Returns the tables of each copy of the driver: the steps, whose first 16 words are 2^((n - 60) / 12) times
// kUnitStep, rounded, then the slides and the vibrato's sine.
std::vector<Tables> FindCopies(const Rom& rom)
{
    std::array<uint32_t, 16> steps = {};
    for (size_t n = 0; n < steps.size(); n++)
    {
        steps[n] = uint32_t(std::lround(kUnitStep * std::exp2((int(n) - kMiddleNote) / 12.0)));
    }

    std::vector<Tables> copies;
    for (const uint32_t at : FindAll(rom, LittleEndian(steps), 4))
    {
        const uint32_t steps_end = at + 4 * kNotes;
        uint32_t fine_up = 0;
        for (uint32_t a = steps_end; a <= steps_end + kMaxStepsGap; a += 2)
        {
            if (Holds(rom, a, kFineUpStart))
            {
                fine_up = a;
                break;
            }
        }

        const uint32_t fine_down = fine_up + kSlideTableBytes;
        const uint32_t vibrato = fine_down + kSlideTableBytes;
        if (fine_up == 0 || !Holds(rom, fine_down, kFineDownStart) || !Holds(rom, vibrato, kVibratoStart))
        {
            continue;
        }

        Tables t;
        t.steps = at;
        t.slide_up = fine_up + kFineSlideBytes;
        t.slide_down = fine_down + kFineSlideBytes;
        t.vibrato = vibrato;
        copies.push_back(t);
    }

    return copies;
}

// Returns the start of each program in the cartridge, in address order: the cartridge's start, and each place whose
// crt0 starts 0xC0 bytes in.
std::vector<uint32_t> FindPrograms(const Rom& rom)
{
    std::vector<uint32_t> programs = {kRomBase};
    for (const uint32_t at : FindAll(rom, kCrt0Start, 4))
    {
        if (at >= kRomBase + kCrt0Offset && at - kCrt0Offset != kRomBase)
        {
            programs.push_back(at - kCrt0Offset);
        }
    }

    return programs;
}

// Returns the place among `programs` of the program that contains `address`: the last that starts at or before it.
int ProgramOf(const std::vector<uint32_t>& programs, uint32_t address)
{
    int found = 0;
    for (size_t i = 0; i < programs.size(); i++)
    {
        if (programs[i] <= address)
        {
            found = int(i);
        }
    }

    return found;
}

// Returns the literal that the load of `sig` reads, or 0 if the code isn't there once.
uint32_t FindLoadedWord(const Rom& rom, const Signature& sig)
{
    const std::span<const uint8_t> bytes = Bytes(rom);
    uint32_t result = 0;
    int matches = 0;
    const size_t span = sig.before.size() + 2 + sig.after.size();
    for (size_t i = 0; i + span <= bytes.size(); i += 2)
    {
        if (!std::equal(sig.before.begin(), sig.before.end(), bytes.begin() + ptrdiff_t(i)))
        {
            continue;
        }

        const size_t load = i + sig.before.size();
        if (bytes[load + 1] != sig.load ||
            !std::equal(sig.after.begin(), sig.after.end(), bytes.begin() + ptrdiff_t(load + 2)))
        {
            continue;
        }

        const uint32_t pc = kRomBase + uint32_t(load);
        const uint32_t literal = ((pc + 4) & ~3u) + uint32_t(bytes[load]) * 4;
        result = rom.U32(literal);
        matches++;
    }

    return matches == 1 ? result : 0;
}

// Whether the pattern at `address` reads to its last row: a count of rows, then rows of entries that name channels 1 to
// 15, each ended by a 0 byte. An entry of mode 1 requires an earlier entry of the pattern for its channel with a mask
// of its own: the packed modules keep a channel's last mask within a pattern as Impulse Tracker's modules do.
bool PatternReads(const Rom& rom, uint32_t address, uint32_t masks)
{
    if (!rom.Contains(address))
    {
        return false;
    }

    const uint8_t rows = rom.U8(address);
    if (rows == 0)
    {
        return false;
    }

    uint32_t a = address + 1;
    std::array<int, kChannels> last_mask = {};
    last_mask.fill(-1);
    for (int row = 0; row < rows; row++)
    {
        int entries = 0;
        while (true)
        {
            if (!rom.Contains(a) || ++entries > kMaxRowEntries)
            {
                return false;
            }

            const uint8_t b = rom.U8(a++);
            if (b == 0)
            {
                break;
            }

            const int channel = (b & 0x0F) - 1;
            const uint8_t mode = b >> 4;
            if (channel < 0)
            {
                return false;
            }

            int mask = 0;
            if (mode == 0)
            {
                mask = rom.U8(a++);
                last_mask[size_t(channel)] = mask;
            }
            else if (mode == 1)
            {
                mask = last_mask[size_t(channel)];
                if (mask < 0)
                {
                    return false;
                }
            }
            else
            {
                mask = rom.U8(masks + mode - 2);
            }

            a += uint32_t((mask & kMaskNote ? 1 : 0) + (mask & kMaskInstrument ? 1 : 0) + (mask & kMaskVolume ? 1 : 0) +
                          (mask & kMaskEffect ? 2 : 0));
        }
    }

    return rom.Contains(a - 1);
}

// The aligned words of the ROM that contain addresses in the ROM, sorted.
std::vector<uint32_t> RomLiterals(const Rom& rom)
{
    std::vector<uint32_t> words;
    for (uint32_t a = kRomBase; a + 4 <= rom.End(); a += 4)
    {
        const uint32_t w = rom.U32(a);
        if (rom.Contains(w))
        {
            words.push_back(w);
        }
    }

    std::sort(words.begin(), words.end());

    return words;
}

// Returns the song tables of `build`: the runs of 2 or more headers that follow one another, and the single headers
// that a word in the ROM names, in address order.
std::vector<std::vector<SongHeader>> FindSongTables(const Rom& rom, Build build, uint32_t samples, uint32_t masks,
                                                    const std::vector<uint32_t>& literals)
{
    const uint32_t size = build == Build::k2002 ? kHeader2002Size : kHeader2003Size;
    std::vector<std::vector<SongHeader>> tables;
    const uint32_t end = rom.End();
    for (uint32_t a = kRomBase; a + size <= end; a += 4)
    {
        // A header's first two words point into the ROM.
        if (!rom.Contains(rom.U32(a)) || !rom.Contains(rom.U32(a + 4)))
        {
            continue;
        }

        std::vector<SongHeader> run;
        uint32_t b = a;
        SongHeader header;
        while (b + size <= end && ReadSongHeader(rom, build, b, samples, masks, header))
        {
            run.push_back(header);
            b += size;
        }

        if (run.empty())
        {
            continue;
        }

        const bool named = std::binary_search(literals.begin(), literals.end(), a);
        if (run.size() >= 2 || named)
        {
            tables.push_back(run);
            a = b - 4;
        }
    }

    return tables;
}

// Returns the samples of each run of the mix routine of the copy of the driver that plays `table`'s songs, from the
// handler's mark, or 0 if the handler's test isn't there.
uint32_t FindMixSamples(const Rom& rom, const SongTable& table)
{
    if (table.build == Build::k2002)
    {
        for (uint32_t a = table.program; a + 6 <= rom.End(); a += 2)
        {
            const uint16_t compare = rom.U16(a);
            if ((compare & 0xFF00) == k2002MarkCompare && (rom.U16(a + 2) & 0xFF00) == 0xD000 &&
                rom.U16(a + 4) == k2002ReturnR6)
            {
                return kRingSamples - (compare & 0xFFu);
            }
        }

        return 0;
    }

    uint32_t source = 0;
    uint32_t size = 0;
    if (!ReadIwramImage(rom, table.program, source, size))
    {
        return 0;
    }

    for (uint32_t a = source; a + 8 <= source + size; a += 4)
    {
        const uint32_t compare = rom.U32(a);
        if ((compare & 0xFFFFFF00) == k2003MarkCompare && rom.U32(a + 4) == k2003ReturnR4)
        {
            // The immediate is rotated right by 10 bits: the mark in the top 10 bits of the lead.
            return kRingSamples - (compare & 0xFFu);
        }
    }

    return 0;
}

// Returns the first match of `pattern` at or after `from`, or 0.
uint32_t FindAfter(const Rom& rom, std::string_view pattern, uint32_t from)
{
    const std::vector<uint32_t> found = agb::FindThumb(rom, agb::ParseThumbPattern(pattern));
    const auto it = std::lower_bound(found.begin(), found.end(), from);

    return it == found.end() ? 0 : *it;
}

// Returns the reload that the later build's init at `init` stores in timer 0 from a literal, or -1 if the store isn't
// there.
int32_t Find2003Reload(const Rom& rom, uint32_t init)
{
    for (uint32_t at = init; at < init + kRoutineReach; at += 2)
    {
        if (!agb::IsThumbLiteralLoad(rom, at) || agb::ThumbLiteral(rom, at) >> 16 != 0xFFFF)
        {
            continue;
        }

        const uint32_t reg = (rom.U16(at) >> 8) & 7u;
        for (uint32_t i = 1; i <= kReloadStoreReach; i++)
        {
            const uint16_t store = rom.U16(at + 2 * i);
            if ((store & 0xFFC7) == (0x8000 | reg))
            {
                return int32_t(agb::ThumbLiteral(rom, at) & 0xFFFF);
            }
        }
    }

    return -1;
}

// Sets the table's mixer check and its warning.
void SetMixerCheck(SongTable& table, MixerCheck check, std::string warning)
{
    table.mixer_check = check;
    table.mixer_warning = std::move(warning);
}

// Checks whether the init of the table's program sets the mixer up as MixerOf() has it for the table's build, and
// warns where it doesn't or where detection can't read how it does.
void CheckMixer(const Rom& rom, SongTable& table, DriverInfo& info)
{
    const std::string where = "the program at " + Hex(table.program);
    const uint32_t init = FindInit(rom, table.build, table.program);
    if (init == 0)
    {
        SetMixerCheck(table, MixerCheck::kUnread,
                      "found no init of the driver in " + where +
                          ". Its songs play at the mixer's rate of the build, which may not be the game's");
        return;
    }

    const Mixer mixer = MixerOf(table.build);
    if (table.build == Build::k2002)
    {
        const uint32_t rate = FindInitRate(rom, init);
        if (rate == 0)
        {
            SetMixerCheck(table, MixerCheck::kUnread,
                          "found no call of the init at " + Hex(init) + " with a rate in " + where +
                              ". Its songs play at the 2002 build's " + std::to_string(k2002Rate) + " Hz");
            return;
        }

        const uint32_t factor = (0x3FFFFFFu + rate) / rate;
        const uint32_t cycles = 0x10000u - ((uint32_t(0) - (factor + 3)) >> 2 & 0xFFFFu);
        info.log.push_back(where + ": init at " + Hex(init) + ", rate " + std::to_string(rate) + " Hz, timer " +
                           std::to_string(cycles) + " cycles");
        if (factor != mixer.step_factor || cycles != mixer.timer_cycles || rate * 5 / 2 != mixer.tick_scale)
        {
            SetMixerCheck(table, MixerCheck::kDiffers,
                          "the game passes a rate of " + std::to_string(rate) + " Hz to the init in " + where +
                              ", not the 2002 build's " + std::to_string(k2002Rate) +
                              ". Its songs play at the wrong pitch and tempo");
        }

        return;
    }

    const int32_t reload = Find2003Reload(rom, init);
    if (reload < 0)
    {
        SetMixerCheck(table, MixerCheck::kUnread,
                      "found no reload of the timer in the init at " + Hex(init) + " in " + where +
                          ". Its songs play at the later build's " + std::to_string(mixer.timer_cycles) +
                          " cycles a sample");
        return;
    }

    const uint32_t cycles = 0x10000u - uint32_t(reload);
    info.log.push_back(where + ": init at " + Hex(init) + ", timer " + std::to_string(cycles) + " cycles");
    if (cycles != mixer.timer_cycles)
    {
        SetMixerCheck(table, MixerCheck::kDiffers,
                      "the init in " + where + " sets the timer to " + std::to_string(cycles) +
                          " cycles a sample, not the later build's " + std::to_string(mixer.timer_cycles) +
                          ". Its songs play at the wrong pitch and tempo");
    }
}

// Sets the table's music volume from kGameVolumes, or warns that the release has no row.
void SetMusicVolume(const Rom& rom, int program, SongTable& table, DriverInfo& info)
{
    const std::string code = rom.GameCode();
    const uint8_t version = rom.Version();
    for (const GameVolume& game : kGameVolumes)
    {
        if (code == game.game_code && version == game.version && program == game.program)
        {
            table.music_volume = game.music_volume;
            return;
        }
    }

    info.warnings.push_back("no music volume for the song table at " + Hex(table.address) +
                            ": the game sets it at run time. The songs play at the init's " +
                            std::to_string(kStartMusicVolume) + " of " + std::to_string(kFullMusicVolume));
}

} // namespace

double Mixer::Rate() const
{
    return kCpuClock / timer_cycles;
}

Mixer MixerOf(Build build)
{
    Mixer m;
    if (build == Build::k2002)
    {
        m.timer_cycles = k2002TimerCycles;
        m.step_factor = k2002StepFactor;
        m.tick_scale = k2002TickScale;
        m.block = k2002Block;
        m.byte_levels = true;
    }
    else
    {
        m.timer_cycles = k2003TimerCycles;
        m.step_factor = k2003StepFactor;
        m.tick_scale = k2003TickScale;
        m.block = k2003Block;
        m.byte_levels = false;
    }

    return m;
}

int DriverInfo::SongCount() const
{
    int count = 0;
    for (const SongTable& table : song_tables)
    {
        count += int(table.songs.size());
    }

    return count;
}

const SongTable* DriverInfo::TableOf(int song, int& index) const
{
    int first = 0;
    for (const SongTable& table : song_tables)
    {
        const int count = int(table.songs.size());
        if (song >= first && song < first + count)
        {
            index = song - first;
            return &table;
        }

        first += count;
    }

    return nullptr;
}

bool ReadIwramImage(const Rom& rom, uint32_t program, uint32_t& source, uint32_t& size)
{
    source = rom.U32(program + kCrt0ImageSource);
    size = rom.U32(program + kCrt0ImageSize);

    return rom.U32(program + kCrt0ImageDestination) == kIwram && size > 0 && rom.Contains(source, size);
}

uint32_t FindMusicVolume(const Rom& rom, Build build, uint32_t program)
{
    return FindAfter(rom, build == Build::k2002 ? k2002Volume : k2003Volume, program);
}

uint32_t FindInit(const Rom& rom, Build build, uint32_t program)
{
    if (build == Build::k2002)
    {
        return FindAfter(rom, k2002Init, program);
    }

    const uint32_t volume = FindMusicVolume(rom, build, program);
    if (volume == 0)
    {
        return 0;
    }

    for (const uint32_t call : agb::FindCalls(rom, volume))
    {
        if (call < program || (rom.U16(call - 2) != kMovsR1Volume && rom.U16(call - 4) != kMovsR1Volume))
        {
            continue;
        }

        for (uint32_t at = call - 2; at + kRoutineReach > call && at > program; at -= 2)
        {
            if ((rom.U16(at) & 0xFF00) == kPushLr && rom.U16(at + 2) == kAddsR4R0)
            {
                return at;
            }
        }
    }

    return 0;
}

uint32_t FindInitRate(const Rom& rom, uint32_t init)
{
    for (const uint32_t call : agb::FindCalls(rom, init))
    {
        if (agb::IsThumbLiteralLoad(rom, call - 2) && (rom.U16(call - 2) & 0x0700) == 0)
        {
            return agb::ThumbLiteral(rom, call - 2);
        }
    }

    return 0;
}

Sample ReadSample(const Rom& rom, uint32_t address)
{
    Sample s;
    s.points = rom.U32(address + kSamplePoints);
    s.flags = rom.U16(address + kSampleFlags);
    s.loop_start = rom.U16(address + kSampleLoopStart);
    s.length = rom.U16(address + kSampleLength);
    s.rate = rom.U16(address + kSampleRate);

    return s;
}

int OrderCount(const Rom& rom, const SongHeader& header)
{
    for (int i = 0; i < kMaxOrders; i++)
    {
        if (!rom.Contains(header.orders + uint32_t(i)) || rom.U8(header.orders + uint32_t(i)) == kOrderEnd)
        {
            return i;
        }
    }

    return kMaxOrders;
}

bool ReadSongHeader(const Rom& rom, Build build, uint32_t address, uint32_t samples, uint32_t masks, SongHeader& header)
{
    header = {};
    header.address = address;
    header.patterns = rom.U32(address + kHeaderPatterns);
    header.orders = rom.U32(address + kHeaderOrders);
    if (build == Build::k2002)
    {
        if (!rom.Contains(address, kHeader2002Size) || rom.U16(address + 10) != 0)
        {
            return false;
        }

        header.samples = samples;
        header.masks = masks;
        header.tempo = rom.U8(address + kHeader2002Tempo);
        header.speed = rom.U8(address + kHeader2002Speed);
    }
    else
    {
        if (!rom.Contains(address, kHeader2003Size) || rom.U16(address + 18) != 0)
        {
            return false;
        }

        header.samples = rom.U32(address + kHeader2003Samples);
        header.masks = rom.U32(address + kHeader2003Masks);
        header.tempo = rom.U8(address + kHeader2003Tempo);
        header.speed = rom.U8(address + kHeader2003Speed);
    }

    // Impulse Tracker's tempos run from 32 to 255.
    if (header.tempo < 32 || header.speed == 0 || header.patterns % 4 != 0 || !rom.Contains(header.patterns, 4) ||
        !rom.Contains(header.orders) || !rom.Contains(header.samples, kSampleSize) ||
        !rom.Contains(header.masks, kTableMasks))
    {
        return false;
    }

    // The orders end within kMaxOrders, and each names a pattern that reads to its last row.
    const int orders = OrderCount(rom, header);
    if (orders == 0 || orders == kMaxOrders || !rom.Contains(header.orders + uint32_t(orders)))
    {
        return false;
    }

    for (int i = 0; i < orders; i++)
    {
        const uint32_t pattern = rom.U32(header.patterns + 4 * uint32_t(rom.U8(header.orders + uint32_t(i))));
        if (!PatternReads(rom, pattern, header.masks))
        {
            return false;
        }
    }

    return true;
}

bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error)
{
    info = {};
    error.clear();

    // The tables that every copy of the driver reads.
    const std::vector<Tables> copies = FindCopies(rom);
    if (copies.empty())
    {
        return false;
    }

    const std::vector<uint32_t> programs = FindPrograms(rom);
    for (const Tables& t : copies)
    {
        info.log.push_back("steps at " + Hex(t.steps) + ", slides up at " + Hex(t.slide_up) + ", down at " +
                           Hex(t.slide_down) + ", vibrato at " + Hex(t.vibrato));
    }

    // The 2002 build's code reads its game-wide tables. The later builds' songs name theirs.
    const uint32_t samples = FindLoadedWord(rom, kSampleTableLoad);
    const uint32_t masks = FindLoadedWord(rom, kMaskTableLoad);
    const bool has_2002 = rom.Contains(samples) && rom.Contains(masks);
    if (has_2002)
    {
        info.log.push_back("2002 build: samples at " + Hex(samples) + ", masks at " + Hex(masks));
    }

    // Each song table takes the copy of the driver in its program.
    const std::vector<uint32_t> literals = RomLiterals(rom);
    for (const Build build : {Build::k2002, Build::k2003})
    {
        if (build == Build::k2002 && !has_2002)
        {
            continue;
        }

        for (std::vector<SongHeader>& songs : FindSongTables(rom, build, samples, masks, literals))
        {
            SongTable table;
            table.address = songs.front().address;
            table.build = build;
            const int program = ProgramOf(programs, table.address);
            table.program = programs[size_t(program)];
            table.songs = std::move(songs);
            const auto copy = std::find_if(copies.begin(), copies.end(),
                                           [&](const Tables& t) { return ProgramOf(programs, t.steps) == program; });
            if (copy == copies.end())
            {
                info.warnings.push_back("the song table at " + Hex(table.address) +
                                        " has no copy of the driver in its program; its songs are left out");
                continue;
            }

            table.tables = *copy;
            table.mix_samples = FindMixSamples(rom, table);
            if (table.mix_samples == 0)
            {
                info.warnings.push_back(
                    "the song table at " + Hex(table.address) +
                    " has no handler of the driver's interrupt in its program; its songs are left out");
                continue;
            }

            SetMusicVolume(rom, program, table, info);
            CheckMixer(rom, table, info);
            if (table.mixer_check != MixerCheck::kSame)
            {
                table.mixer_warning_index = info.warnings.size();
                info.warnings.push_back(table.mixer_warning);
            }

            info.song_tables.push_back(std::move(table));
        }
    }

    std::sort(info.song_tables.begin(), info.song_tables.end(),
              [](const SongTable& a, const SongTable& b) { return a.address < b.address; });

    if (info.song_tables.empty())
    {
        error = "found the driver's tables but no song table";
        return false;
    }

    for (const SongTable& table : info.song_tables)
    {
        info.log.push_back(std::string(table.build == Build::k2002 ? "2002" : "2003") + " build: song table at " +
                           Hex(table.address) + ", " + std::to_string(table.songs.size()) + " songs, program at " +
                           Hex(table.program) + ", " + std::to_string(table.mix_samples) +
                           " samples a mix, music volume " + std::to_string(table.music_volume) + " of " +
                           std::to_string(kFullMusicVolume));
    }

    return true;
}

} // namespace agblib::webfoot
