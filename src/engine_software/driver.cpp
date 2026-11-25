// SPDX-License-Identifier: MIT

#include "agblib/engine_software/driver.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/arm.h"
#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/thumb.h"

namespace agblib::engine_software
{
namespace
{

using agblib::agb::ArmPattern;
using agblib::agb::FindArm;
using agblib::agb::FindCalls;
using agblib::agb::Hex;
using agblib::agb::IsThumbLiteralLoad;
using agblib::agb::kRomBase;
using agblib::agb::MatchesThumb;
using agblib::agb::ThumbLiteral;

// The mono mixer's handler of a looping channel: it loads the channel's volume, end, fraction, step and place, adds
// the sample times the volume to the sum, steps, and goes back by the loop's bytes past the end.
constexpr ArmPattern kHandlerCode[] = {{0xE9111F00}, {0xE08AA00B}, {0xE1DCB0D0}, {0xE026689B},
                                       {0xE141A0BC}, {0xE08CC82A}, {0xE15C0009}, {0xA511901C},
                                       {0xA04CC009}, {0xE501C004}, {0xE491804C}};

// The rates that the init tests for, with the samples a frame and the step's factor that it sets for each, and the most
// that it accepts.
constexpr uint32_t kRateLow = 0x1666;
constexpr uint32_t kRateMiddle = 0x2910;
constexpr uint32_t kRateHigh = 0x3442;

// The instructions that load the bank before the bank's load: the call follows at most this many halfwords after the
// literal's load.
constexpr uint32_t kBankLiteralReach = 8;

// The limits of a song's header that a read accepts.
constexpr uint32_t kMaxRows = 256;

// The first instructions of each routine that a build lists, in ParseThumbPattern()'s form.
struct RoutineCode
{
    const char* name;
    uint32_t Routines::* routine;
    const char* code;
};

constexpr RoutineCode kRoutineCode[] = {
    {"reset", &Routines::reset, "B530 B081 4A15 4B16 6013 23C0"},
    {"init", &Routines::init, "B570 1C05 1C0E 1C14 F000 F848"},
    {"bank load", &Routines::bank_load, "B530 4D0D 61E8 1C04 6A29 2900"},
    {"effect set", &Routines::effect_set, "B530 1C04 4D09 77AC 4809 6001"},
    {"play by number", &Routines::play_number, "B500 1C01 4807 060A 69C0 0D92"},
    {"music volume", &Routines::music_volume, "B570 4B0D 4A0D 2100 8251 82D0"},
    {"frame", &Routines::frame, "B5F0 4F16 8839 2004 4008 2800"},
    {"swap", &Routines::swap, "B500 4A05 8A11 2001 4008 2800"},
};

struct ManagerCode
{
    const char* name;
    uint32_t Manager::* routine;
    const char* code;
};

constexpr ManagerCode kManagerCode[] = {
    {"set music volume", &Manager::set_music_volume, "B530 1C0C 6805 6868 42A0 D003"},
    {"set effect volume", &Manager::set_effect_volume, "B530 1C0C 6805 6828 42A0 D003"},
    {"start", &Manager::start, "B5F0 464F 4646 B4C0 4680 4689"},
    {"jingle", &Manager::jingle, "B500 2080 0040 4308 F7D4 FB06"},
    {"vertical blank", &Manager::vblank, "B500 6801 2000 7608 F7D4 F958"},
    {"vertical count", &Manager::vcount, "B510 1C04 6820 7E00 2800 D10A"},
    {"music ramp", &Manager::music_ramp, "B530 1C02 6811 6888 2800 D015"},
};

// The Santa Clause 3's build. The addresses come from a reading of the game's code. The game's sound manager (methods
// 0x0802CA84 to 0x0802CFBC) sets up the driver once: the reset with the allocator 0x0802CFBD (fixed buffers in IWRAM),
// the init with mono output at 13,378 Hz and the default gain, the bank's load and 2 effect channels on the table
// 0x0871D8C0. Its start (0x0802CBE8) takes the music volume 0xD2 (the word at 0x08055CA4) and a ramp of 0x40 frames, of
// 1 for song 6 (callers 0x0800D908, 0x080223EA and 0x08025F3E). Each caller enters a state of the player. The state
// switches to a level or a screen 120 frames after its animation ends, and the switch fades the music out (0x0802CDD0).
// Runs in mGBA 0.11.0 entered the three states through the player's switch of states (0x08007220) in a live level. The
// fade started 175 or 200 frames after song 6's start, and the driver first returns to the restart order 245 frames
// after it. Song 6 therefore plays one pass. Song 5 is a jingle: the game plays it with 0x0802CC7C over the song that
// plays (0x0800FD26, 0x0802546C, 0x08028B02 and 0x0802A2CE). The crt0 stores WAITCNT 0x4014, and the game then stores
// 0x4314 every few frames (0x0803EAFC). IE 0x2005 and DISPSTAT 0x1B28: the vertical blank runs the manager's routine
// 0x0802CEB8 and the vertical count at line 27 its 0x0802CEC8.
Build SantaClause3()
{
    Build b;
    b.code = "B33E";
    b.name = "The Santa Clause 3: The Escape Clause";
    b.supported = true;
    b.handler = 0x08001890;
    b.routines = {.reset = 0x0800102C,
                  .init = 0x08001108,
                  .bank_load = 0x0800122C,
                  .effect_set = 0x080013E0,
                  .play_number = 0x08001294,
                  .music_volume = 0x08001334,
                  .frame = 0x08000C14,
                  .swap = 0x08001174,
                  .mixer = 0x08001844,
                  .code_start = 0x08000218,
                  .code_end = 0x080025F0};
    b.manager = {.set_music_volume = 0x0802CE38,
                 .set_effect_volume = 0x0802CDAC,
                 .start = 0x0802CBE8,
                 .jingle = 0x0802CC7C,
                 .vblank = 0x0802CEB8,
                 .vcount = 0x0802CEC8,
                 .music_ramp = 0x0802CAC4,
                 .block_size = 0x28,
                 .music_volume = 0xD2,
                 .effect_volume = 0x100,
                 .ramp_frames = 0x40,
                 .object = 0x02000000};
    b.mixer_state = 0x030003D4;
    b.channels = 0x03000778;
    b.sequencer = 0x03000A74;
    b.sine = 0x080484E8;
    b.pitch = 0x08048568;
    b.pitch_words = 0x0804DA10;
    b.allocator = 0x0802CFBD;
    b.free = 0x0802CFB9;
    b.rate = kRateHigh;
    b.effect_channels = 2;
    b.effect_table = 0x0871D8C0;
    b.music_channels = 8;
    b.waitcnt = 0x4314;
    b.dispstat = 0x1B28;
    b.interrupts = 0x2005;
    b.starts.assign(12, SongStart{.frames = 0x40});
    b.starts[5] = {.jingle = true};
    b.starts[6] = {.frames = 1, .one_pass = true};
    return b;
}

// The builds that the model doesn't follow yet. Their songs use effects that B33E's songs don't, and Dragon Ball Z:
// Taiketsu plays at 10,512 Hz with a song of 13 channels.
Build Unsupported(const char* code, const char* name, uint32_t handler)
{
    Build b;
    b.code = code;
    b.name = name;
    b.handler = handler;
    return b;
}

// Returns the bank's address from the game's calls of the bank's load: the literal that each call loads into r0 just
// before it. Returns 0 unless every call loads the same address.
uint32_t BankFromCalls(const Rom& rom, uint32_t bank_load)
{
    uint32_t bank = 0;
    for (const uint32_t call : FindCalls(rom, bank_load))
    {
        uint32_t found = 0;
        for (uint32_t back = 1; back <= kBankLiteralReach && found == 0; back++)
        {
            const uint32_t at = call - 2 * back;
            if (IsThumbLiteralLoad(rom, at) && (rom.U16(at) & 0x0700) == 0)
            {
                found = ThumbLiteral(rom, at);
            }
        }

        if (found == 0 || (bank != 0 && found != bank))
        {
            return 0;
        }

        bank = found;
    }

    return bank;
}

// Reads the song at `address` and its patterns.
bool ReadSong(const Rom& rom, uint32_t address, Song& song, std::string& error)
{
    if (!rom.Contains(address, kSongOrderList))
    {
        error = "song at " + Hex(address) + " is outside the cartridge";
        return false;
    }

    song.address = address;
    song.channels = rom.U8(address + kSongChannels);
    song.orders = rom.U8(address + kSongOrders);
    song.restart = rom.U8(address + kSongRestart);
    song.pattern_count = rom.U8(address + kSongPatterns);
    song.speed = rom.U8(address + kSongSpeed);
    song.tempo = rom.U8(address + kSongTempo);
    song.order_list = address + kSongOrderList;
    if (song.channels == 0 || song.orders == 0 || song.pattern_count == 0)
    {
        error = "song at " + Hex(address) + " has no channels, orders or patterns";
        return false;
    }

    // The patterns follow the order list, padded to a word: each is its count of rows, a halfword 0 and a word a row.
    uint32_t pattern = song.order_list + ((uint32_t(song.orders) + 3) & ~3u);
    for (int p = 0; p < song.pattern_count; p++)
    {
        const uint32_t rows = rom.Contains(pattern, kPatternRows) ? rom.U16(pattern) : 0;
        if (rows == 0 || rows > kMaxRows || !rom.Contains(pattern, kPatternRows + 4 * rows))
        {
            error = "song at " + Hex(address) + " has a pattern at " + Hex(pattern) + " that can't be read";
            return false;
        }

        song.patterns.push_back(pattern);
        pattern += kPatternRows + 4 * rows;
    }

    for (int o = 0; o < song.orders; o++)
    {
        if (rom.U8(song.order_list + uint32_t(o)) >= song.pattern_count)
        {
            error = "song at " + Hex(address) + " plays a pattern it doesn't have";
            return false;
        }
    }

    return true;
}

} // namespace

const std::vector<Build>& Builds()
{
    static const std::vector<Build> kBuilds = {
        SantaClause3(),
        Unsupported("BDBE", "Dragon Ball Z: Taiketsu", 0x08001BB8),
        Unsupported("BTYE", "Ty the Tasmanian Tiger 2: Bush Rescue", 0x080B7060),
    };
    return kBuilds;
}

uint32_t FrameSamplesAt(uint32_t rate)
{
    switch (rate)
    {
    case kRateLow:
        return 96;
    case kRateMiddle:
        return 176;
    case kRateHigh:
        return 224;
    default:
        return 0;
    }
}

uint32_t StepFactorAt(uint32_t rate)
{
    switch (rate)
    {
    case kRateLow:
        return 0xB6E;
    case kRateMiddle:
        return 0x63C;
    case kRateHigh:
        return 0x4E6;
    default:
        return 0;
    }
}

uint32_t StoredRate(uint32_t rate)
{
    return rate > kRateHigh ? kRateHigh : rate;
}

uint32_t TickSamples(uint32_t rate, uint8_t tempo)
{
    const uint32_t divisor = (uint32_t(tempo) * 0x333u * 0x1001u) >> 23;
    return divisor == 0 ? 0 : rate / divisor;
}

uint32_t DriverInfo::FrameSamples() const
{
    return FrameSamplesAt(StoredRate(build.rate));
}

uint32_t DriverInfo::StepFactor() const
{
    return StepFactorAt(StoredRate(build.rate));
}

double DriverInfo::OutputRate() const
{
    // The timer counts the CPU clock's cycles from 0x10000 less 2^24 over the rate.
    const uint32_t stored = StoredRate(build.rate);
    return stored == 0 ? 0 : double(agb::kSecondCycles) / double(0x1000000u / stored);
}

std::vector<GameStep> SetUpSteps(const DriverInfo& info)
{
    const Build& b = info.build;
    const uint32_t object = b.manager.object;
    const uint32_t block = object + kManagerBlock;
    std::vector<GameStep> steps;
    for (uint32_t at = 0; at < b.manager.block_size; at += 4)
    {
        steps.push_back({.address = block + at, .values = {0}, .size = 4});
    }

    steps.push_back({.address = object, .values = {block}, .size = 4});
    steps.push_back({.address = b.manager.set_music_volume | 1, .values = {object, b.manager.music_volume}});
    steps.push_back({.address = b.manager.set_effect_volume | 1, .values = {object, b.manager.effect_volume}});
    steps.push_back({.address = block + kManagerFlag, .values = {1}, .size = 1});
    steps.push_back({.address = b.routines.reset | 1, .values = {b.allocator, b.free}});
    steps.push_back({.address = b.routines.init | 1, .values = {b.stereo ? 1u : 0u, b.rate, b.gain}});
    steps.push_back({.address = b.routines.bank_load | 1, .values = {info.bank.address}});
    steps.push_back({.address = b.routines.effect_set | 1, .values = {b.effect_channels, b.effect_table}});

    return steps;
}

std::vector<GameStep> StartSteps(const DriverInfo& info, int song)
{
    const Manager& m = info.build.manager;
    const SongStart& start = info.songs.at(size_t(song)).start;
    if (start.jingle)
    {
        // A jingle plays over a song at the manager's music volume.
        return {{.address = info.build.routines.music_volume | 1, .values = {m.music_volume}},
                {.address = m.jingle | 1, .values = {m.object, uint32_t(song)}}};
    }

    return {{.address = m.start | 1, .values = {m.object, uint32_t(song), start.frames, m.music_volume}}};
}

uint32_t PitchEntry(const Build& build, int32_t index)
{
    const uint32_t i = uint32_t(index);
    return i < kPitchHalfwords ? build.pitch + 2 * i : build.pitch_words + 4 * (i - kPitchHalfwords);
}

uint32_t PitchRate(const Rom& rom, const Build& build, int32_t index)
{
    const uint32_t at = PitchEntry(build, index);
    return uint32_t(index) < kPitchHalfwords ? rom.U16(at) : rom.U32(at);
}

uint32_t PitchStep(const Rom& rom, const DriverInfo& info, int32_t index)
{
    return (info.StepFactor() * PitchRate(rom, info.build, index)) >> 8;
}

uint32_t InstrumentEnd(const Rom& rom, uint32_t record)
{
    return record + ((rom.U32(record + kInstrumentLength) + kInstrumentSamples + 3) & ~3u);
}

bool ReadBank(const Rom& rom, uint32_t address, Bank& bank, std::vector<Song>& songs, std::string& error)
{
    if (!rom.Contains(address, kBankSongTable) || rom.U8(address + kBankVersion) != kBankVersionOne)
    {
        error = "no bank of set version 1 at " + Hex(address);
        return false;
    }

    bank.address = address;
    bank.kind = rom.U8(address + kBankKind);
    const uint32_t song_count = rom.U8(address + kBankSongs);
    const uint32_t instrument_count = rom.U8(address + kBankInstruments);
    if (!rom.Contains(address + kBankSongTable, 4 * song_count))
    {
        error = "the bank at " + Hex(address) + " has a song table outside the cartridge";
        return false;
    }

    // The instruments follow the song table, each with its samples.
    uint32_t record = address + kBankSongTable + 4 * song_count;
    for (uint32_t i = 0; i < instrument_count; i++)
    {
        if (!rom.Contains(record, kInstrumentSamples) || !rom.Contains(record, InstrumentEnd(rom, record) - record))
        {
            error =
                "the bank at " + Hex(address) + " has an instrument at " + Hex(record) + " that leaves the cartridge";
            return false;
        }

        bank.instruments.push_back(record);
        record = InstrumentEnd(rom, record);
    }

    for (uint32_t s = 0; s < song_count; s++)
    {
        const uint32_t offset = rom.U32(address + kBankSongTable + 4 * s);
        bank.song_offsets.push_back(offset);
        Song song;
        if (!ReadSong(rom, address + offset, song, error))
        {
            error = "song " + std::to_string(s) + ": " + error;
            return false;
        }

        songs.push_back(std::move(song));
    }

    return true;
}

bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    const uint32_t handler = FindArm(rom, kRomBase, rom.End(), kHandlerCode);
    if (handler == 0)
    {
        return false;
    }

    const Build* build = nullptr;
    for (const Build& b : Builds())
    {
        if (rom.GameCode() == b.code && handler == b.handler)
        {
            build = &b;
        }
    }

    if (build == nullptr)
    {
        error = "Engine Software's driver: the mixer at " + Hex(handler) + " is in a build that isn't known";
        return false;
    }

    if (!build->supported)
    {
        error = std::string("Engine Software's driver: the build of ") + build->name + " (" + build->code +
                ") isn't supported yet";
        return false;
    }

    // The build's routines and the manager's methods have to start with their code.
    for (const RoutineCode& r : kRoutineCode)
    {
        if (!MatchesThumb(rom, build->routines.*r.routine, r.code))
        {
            error = std::string("Engine Software's driver: no ") + r.name + " routine at " +
                    Hex(build->routines.*r.routine);
            return false;
        }
    }

    for (const ManagerCode& m : kManagerCode)
    {
        if (!MatchesThumb(rom, build->manager.*m.routine, m.code))
        {
            error = std::string("Engine Software's driver: the sound manager has no ") + m.name + " routine at " +
                    Hex(build->manager.*m.routine);
            return false;
        }
    }

    info.build = *build;
    const uint32_t bank = BankFromCalls(rom, build->routines.bank_load);
    if (bank == 0)
    {
        error = "Engine Software's driver: no call of the bank's load at " + Hex(build->routines.bank_load) +
                " names one bank";
        return false;
    }

    if (!ReadBank(rom, bank, info.bank, info.songs, error))
    {
        error = "Engine Software's driver: " + error;
        return false;
    }

    if (info.songs.size() != build->starts.size())
    {
        error = "Engine Software's driver: the bank at " + Hex(bank) + " has " + std::to_string(info.songs.size()) +
                " songs, and the game's table " + std::to_string(build->starts.size());
        return false;
    }

    for (size_t s = 0; s < info.songs.size(); s++)
    {
        info.songs[s].start = build->starts[s];
    }

    info.log.push_back(std::string("build of ") + build->name + " (" + build->code + "), mixer handler at " +
                       Hex(handler));
    info.log.push_back("bank at " + Hex(bank) + ": " + std::to_string(info.bank.instruments.size()) + " instruments, " +
                       std::to_string(info.songs.size()) + " songs");
    return true;
}

} // namespace agblib::engine_software
