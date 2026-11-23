// SPDX-License-Identifier: MIT

#include "agblib/frontline_xm/driver.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "agblib/agb/arm.h"
#include "agblib/agb/rom.h"

namespace agblib::frontline_xm
{
namespace
{

using agblib::agb::ArmPattern;
using agblib::agb::Hex;
using agblib::agb::kRomBase;

// The mix routine's first instructions: it saves lr, makes room for its locals, loads the state's address, keeps the
// master volume, and reads the samples a frame, the samples left to the tick, and the output and sum buffers.
constexpr ArmPattern kMixCode[] = {{0xE92D4000}, {0xE24DD050}, {0xE59FA0BC}, {0xE5DA900A}, {0xE1CD94B2},
                                   {0xE1DA90B0}, {0xE1DA80B2}, {0xE59A7058}, {0xE59A6064}, {0xE3580000}};

// The other routines, by their distance from the mix routine and their first instructions.
struct Routine
{
    const char* name;
    int32_t offset;
    std::array<uint32_t, 6> code;
};

constexpr Routine kRoutines[] = {
    {"load", -0x5DF8, {0xE92D4000, 0xE92D1FF0, 0xE1A0A000, 0xE92D000F, 0xE3A00000, 0xEB0001BE}},
    {"play", -0x5568, {0xE92D4000, 0xE92D0030, 0xE59F303C, 0xE5830048, 0xE5C31039, 0xE5C3203A}},
    {"volume", -0x5408, {0xE3E02000, 0xE3500000, 0xBA000004, 0xE3500F40, 0xCA000002, 0xE59F3024}},
    {"music", -0x56E4, {0xE92D4000, 0xE3500000, 0x0A000004, 0xE59F1074, 0xE5D10009, 0xE3800001}},
    {"output", -0x55B4, {0xE92D4000, 0xE59F300C, 0xE3A01001, 0xE5C31007, 0xE8BD4000, 0xE12FFF1E}},
    {"frame", -0x5B04, {0xE92D4000, 0xE92D1FFF, 0xE59FC038, 0xE5DC2007, 0xE3520000, 0x0A000008}},
    {"swap", -0x5B8C, {0xE92D4000, 0xE59F3074, 0xE5D30007, 0xE3500000, 0x0A00000E, 0xE5930058}},
    {"tick", 0xD0, {0xE92D4000, 0xE59FA02C, 0xE5DA9034, 0xE5DA8035, 0xE1590008, 0xBA000003}},
    {"row", 0x10C, {0xE92D4000, 0xE59A9044, 0xE59F7160, 0xE5DA603B, 0xE59F215C, 0xE5DA500C}},
    {"segment", -0x53C4, {0xE92D4000, 0xE24DD00C, 0xE59FC094, 0xE5DCC03B, 0xE59FB090, 0xE3A0C004}},
};

// The literal words that give the tables, by their distance from the mix routine.
constexpr int32_t kLiteralState = 0xCC;     // in the mix routine
constexpr int32_t kLiteralChannels = 0x27C; // in the row routine
constexpr int32_t kLiteralSteps = -0x5318;  // in the segment's set-up
constexpr int32_t kLiteralPeriods = -0x5314;
constexpr int32_t kLiteralStepRates = -0x5310;
constexpr int32_t kLiteralFrameSamples = -0x5308;
constexpr int32_t kLiteralRates = -0x5D18; // in the load
constexpr int32_t kLiteralSine = -0x5D14;
constexpr int32_t kLiteralSaw = 0x824;       // in the handler of the vibrato's wave
constexpr int32_t kLiteralSilence = -0x5798; // in the mixer's code that the player copies to IWRAM

// The step's product with the rate's factor keeps bits 18 to 49.
constexpr int kStepShift = 18;

// The limits of a module's header that a read accepts.
constexpr uint32_t kMaxRows = 256;

// The records of a game's level table: the order list, the last order + 1, and the master volume.
constexpr uint32_t kLevelList = 0x14;
constexpr uint32_t kLevelCount = 0x20;
constexpr uint32_t kLevelVolume = 0x21;

// A game's set-up and its songs, read from its code. A game is named by its code and version.
struct Game
{
    const char* code;
    uint8_t version;
    uint32_t sound_init; // Thumb
    uint8_t rate_index;
    uint32_t title_module;
    uint32_t title_list;
    uint8_t title_last;
    uint8_t title_volume;
    uint32_t level_module;
    uint32_t level_table;
    uint32_t level_count;
};

// The games. The set-up is the game's routine that sets up the player at boot: the player's init, its buffers, the
// rate's index 6 (20,068 Hz) and the options' volumes. The title calls the load with the first module, plays its list
// to order 6 and passes the title's volume, 224, times the options' music volume over 9. A level calls the routine that
// loads the second module, plays the level record's list to its count less 1 and passes the record's volume the same
// way. A new save has the options' music volume at 9. The master volume is then the title's or the level's.
//
// Chicken Shoot: the set-up at 0x08014948, the title at 0x080149EC, the levels through 0x08014D1C from 0x080050D8,
// with the table at 0x083DAE34 (7 records, from 0x08004DB0). Chicken Shoot 2: the set-up at 0x08014A00, the title at
// 0x08014AA4, the levels through 0x08014DD4 from 0x080050F0, with the table at 0x083BC5B0 (from 0x08004DC8).
const std::vector<Game>& Games()
{
    static const std::vector<Game> kGames = {
        {"B6FP", 0, 0x08014949, 6, 0x0801A208, 0x083DF0D8, 6, 224, 0x0803227C, 0x083DAE34, 7},
        {"B6GE", 0, 0x08014A01, 6, 0x0801A2C0, 0x083C0854, 6, 224, 0x08032334, 0x083BC5B0, 7},
    };
    return kGames;
}

// Finds the library around the mix routine and reads its tables.
bool FindLibrary(const Rom& rom, Library& library, std::string& error)
{
    const uint32_t mix = agb::FindArm(rom, kRomBase, rom.End(), kMixCode);
    if (mix == 0)
    {
        return false;
    }

    for (const Routine& routine : kRoutines)
    {
        const uint32_t at = uint32_t(int64_t(mix) + routine.offset);
        std::vector<ArmPattern> code;
        for (const uint32_t word : routine.code)
        {
            code.push_back({word});
        }

        if (!agb::MatchesArm(rom, at, code))
        {
            error = "Frontline Studios' XM-like player: the mix routine at " + Hex(mix) + " has no " + routine.name +
                    " routine at " + Hex(at);
            return false;
        }
    }

    const auto at = [&](int32_t offset)
    {
        return uint32_t(int64_t(mix) + offset);
    };

    const auto literal = [&](int32_t offset)
    {
        return rom.U32(at(offset));
    };

    library.mix = mix;
    library.load = at(kRoutines[0].offset);
    library.play = at(kRoutines[1].offset);
    library.volume = at(kRoutines[2].offset);
    library.music = at(kRoutines[3].offset);
    library.output = at(kRoutines[4].offset);
    library.frame = at(kRoutines[5].offset);
    library.swap = at(kRoutines[6].offset);
    library.state = literal(kLiteralState);
    library.channels = literal(kLiteralChannels);
    library.steps = literal(kLiteralSteps);
    library.periods = literal(kLiteralPeriods);
    library.step_rates = literal(kLiteralStepRates);
    library.frame_samples = literal(kLiteralFrameSamples);
    library.rates = literal(kLiteralRates);
    library.silence = literal(kLiteralSilence);
    const uint32_t sine = literal(kLiteralSine);
    library.waves = {sine, sine + kWaveSize, literal(kLiteralSaw)};

    for (const uint32_t table : {library.steps, library.periods, library.step_rates, library.frame_samples,
                                 library.rates, library.silence, library.waves[0], library.waves[2]})
    {
        if (!rom.Contains(table, 4))
        {
            error = "Frontline Studios' XM-like player: a table of the player at " + Hex(mix) + " is outside the ROM";
            return false;
        }
    }

    return true;
}

} // namespace

uint32_t DriverInfo::FrameSamples(const Rom& rom) const
{
    return rom.U16(library.frame_samples + 2 * rate_index);
}

uint32_t DriverInfo::Rate(const Rom& rom) const
{
    return rom.U16(library.rates + 2 * rate_index);
}

uint32_t StepOf(const Rom& rom, const DriverInfo& info, uint32_t period)
{
    const Library& lib = info.library;
    const uint32_t factor = rom.U32(lib.step_rates + 4 * uint32_t(info.rate_index));
    const uint16_t place = rom.U16(lib.periods + 2 * period);
    const uint32_t base = rom.U32(lib.steps + 4 * uint32_t(place >> 4)) >> (place & 15);
    return uint32_t((uint64_t(base) * factor) >> kStepShift);
}

bool ReadModule(const Rom& rom, uint32_t address, Module& module)
{
    if (!rom.Contains(address, 0x14))
    {
        return false;
    }

    module.address = address;
    module.channels = rom.U8(address + kHeaderChannels);
    module.pattern_count = rom.U8(address + kHeaderPatternCount);
    module.master = rom.U8(address + kHeaderMaster);
    module.note_handler = rom.U8(address + kHeaderNoteHandler);
    module.last_order = rom.U8(address + kHeaderLastOrder);
    module.speed = rom.U8(address + kHeaderSpeed);
    module.bpm = rom.U8(address + kHeaderBpm);
    module.orders = rom.U32(address + kHeaderOrders);
    module.patterns = rom.U32(address + kHeaderPatterns);
    module.instruments = rom.U32(address + kHeaderInstruments);
    if (module.channels == 0 || module.channels > kChannels || module.bpm == 0 ||
        !rom.Contains(module.orders, uint32_t(module.last_order) + 1) || !rom.Contains(module.patterns, 4) ||
        !rom.Contains(module.instruments, 4))
    {
        return false;
    }

    // Each pattern of the list reads to its last row.
    for (uint32_t i = 0; i < module.last_order; i++)
    {
        const uint32_t pattern = rom.U32(module.patterns + 4 * rom.U8(module.orders + i));
        if (!rom.Contains(pattern, kPatternCells))
        {
            return false;
        }

        const uint32_t rows = rom.U16(pattern);
        if (rows == 0 || rows > kMaxRows)
        {
            return false;
        }

        uint32_t at = pattern + kPatternCells;
        for (uint32_t cell = 0; cell < rows * module.channels; cell++)
        {
            if (!rom.Contains(at, 1))
            {
                return false;
            }

            const uint8_t flags = rom.U8(at++);
            at += (flags & kCellNote ? 2 : 0) + (flags & kCellVolume ? 1 : 0) + (flags & kCellEffect ? 2 : 0);
        }
    }

    return true;
}

bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error)
{
    error.clear();
    info = {};
    if (!FindLibrary(rom, info.library, error))
    {
        return false;
    }

    const std::string code = rom.GameCode();
    const uint8_t version = rom.Version();
    const auto game = std::find_if(Games().begin(), Games().end(),
                                   [&](const Game& g) { return code == g.code && version == g.version; });
    if (game == Games().end())
    {
        error = "Frontline Studios' XM-like player: the game's set-up of the player isn't known for game code " + code;
        return false;
    }

    info.sound_init = game->sound_init;
    info.rate_index = game->rate_index;
    for (const uint32_t address : {game->title_module, game->level_module})
    {
        Module module;
        if (!ReadModule(rom, address, module))
        {
            error = "Frontline Studios' XM-like player: the module at " + Hex(address) + " can't be read";
            return false;
        }

        info.modules.push_back(module);
    }

    // The title's list, then each distinct list of the level table.
    info.songs.push_back({0, game->title_list, game->title_last, game->title_volume});
    std::set<std::tuple<uint32_t, uint8_t, uint8_t>> seen;
    for (uint32_t i = 0; i < game->level_count; i++)
    {
        const uint32_t record = rom.U32(game->level_table + 4 * i);
        if (!rom.Contains(record, kLevelVolume + 1))
        {
            error = "Frontline Studios' XM-like player: the level table at " + Hex(game->level_table) +
                    " names a record outside the ROM";
            return false;
        }

        const uint32_t list = rom.U32(record + kLevelList);
        const uint8_t count = rom.U8(record + kLevelCount);
        const uint8_t volume = rom.U8(record + kLevelVolume);
        if (count == 0 || !rom.Contains(list, count))
        {
            error = "Frontline Studios' XM-like player: the level record at " + Hex(record) + " has no list";
            return false;
        }

        if (seen.insert({list, uint8_t(count - 1), volume}).second)
        {
            info.songs.push_back({1, list, uint8_t(count - 1), volume});
        }
    }

    const Library& lib = info.library;
    info.log.push_back("mix " + Hex(lib.mix) + ", load " + Hex(lib.load) + ", play " + Hex(lib.play) + ", state " +
                       Hex(lib.state) + ", channels " + Hex(lib.channels) + ", mixer " +
                       std::to_string(info.Rate(rom)) + " Hz, " + std::to_string(info.FrameSamples(rom)) +
                       " samples a frame");
    for (size_t s = 0; s < info.songs.size(); s++)
    {
        const Song& song = info.songs[s];
        info.log.push_back("song " + std::to_string(s) + ": module " + Hex(info.modules[size_t(song.module)].address) +
                           ", the list at " + Hex(song.list) + " to order " + std::to_string(song.last) +
                           ", master volume " + std::to_string(song.volume) + (s == 0 ? " (the title)" : ""));
    }

    return true;
}

} // namespace agblib::frontline_xm
