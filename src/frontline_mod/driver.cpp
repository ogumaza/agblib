// SPDX-License-Identifier: MIT

#include "agblib/frontline_mod/driver.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "agblib/agb/arm.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"

namespace agblib::frontline_mod
{
namespace
{

using agblib::agb::ArmPattern;
using agblib::agb::Hex;
using agblib::agb::kIwramBase;
using agblib::agb::kIwramSize;
using agblib::agb::kRomBase;

// The smallest parameter of the clip table's builder.
constexpr uint32_t kMinClipScale = 16;

// The player's first instructions: it saves the registers, loads the state's address and returns at once when the
// player is off, then counts the tick up to the speed.
constexpr ArmPattern kPlayerCode[] = {{0xE92D5FFF}, {0xE59F0560}, {0xE4901004}, {0xE3510000},
                                      {0x0A000063}, {0xE590A00C}, {0xE5901000}, {0xE5902004},
                                      {0xE2811001}, {0xE1510002}, {0xAA00000E}, {0xE5801000}};

// The mixer's first instructions: it saves the registers, then sets each pair of sums to the bias.
constexpr ArmPattern kMixerCode[] = {{0xE92D5FFF}, {0xE590C004}, {0xE59FA160}, {0xE59FB180},
                                     {0xE89B03FC}, {0xE8AA03FC}, {0xE25CC010}, {0x1AFFFFFC},
                                     {0xE58A2000}, {0xE8901F80}, {0xE59F213C}, {0xE5829000}};

// The words of the player's literal pool, from the player's start.
constexpr uint32_t kPlayerEffects = 0x568;
constexpr uint32_t kPlayerState = 0x56C;
constexpr uint32_t kPlayerContext = 0x570;
constexpr uint32_t kPlayerPeriods = 0x574;
constexpr uint32_t kPlayerVibrato = 0x578;

// The effect table's distance from the player's start.
constexpr uint32_t kEffectsOffset = 0x4E8;

// The words of the mixer's literal pool, from the mixer's start.
constexpr uint32_t kMixerClipTable = 0x160;
constexpr uint32_t kMixerOutput = 0x164;
constexpr uint32_t kMixerAccumulator = 0x170;

// How far into the ROM the start-up code's words of the copy to IWRAM can be.
constexpr uint32_t kStartupWords = 0x400;

// The limits of a module's header that a read accepts.
constexpr uint32_t kMaxChannels = 16;
constexpr uint32_t kMaxOrders = 256;
constexpr uint32_t kMaxPatterns = 256;
constexpr uint32_t kMaxRows = 256;
constexpr uint32_t kMaxSpeed = 31;

// A game's set-up of the driver and its songs, read from its code. A game is named by its code and version.
// A start of a module by the game's code: the order and the fade step.
struct GameStart
{
    uint32_t order = 0;
    int32_t step = kFullVolume;
};

struct GameModule
{
    uint32_t address = 0;
    std::vector<GameStart> starts;
};

struct Game
{
    const char* code;
    uint8_t version;
    std::vector<GameCall> init;
    uint32_t start = 0;
    MixerSettings mixer;
    std::vector<GameModule> modules;
    int32_t unstarted_step = kFullVolume; // the step of the songs that the game never starts: its most common one
};

// Returns the starts of `orders`, each with the fade step `step`.
std::vector<GameStart> Starts(std::initializer_list<uint32_t> orders, int32_t step)
{
    std::vector<GameStart> starts;
    for (const uint32_t order : orders)
    {
        starts.push_back({order, step});
    }

    return starts;
}

// The games. Each one's set-up comes from its call of the driver's init, and its songs from its calls of the start
// and of the routine that moves the playing module to another order.
const std::vector<Game>& Games()
{
    static const std::vector<Game> kGames = {
        // Hardcore Pool. The game's main routine makes the heap in IWRAM that the mixer's init takes its channels from
        // (0x08037410: 0x080393CC with the heap's word 0x030031D4, 0x03004060 and the bytes up to 0x03007C00). The
        // game's init (0x080002A6) sets up the mixer (0x08035B88: 8 channels, 304 samples a frame, master volume 0x50)
        // and the sound's registers (0x08035C00). It starts the module at order 0 with a fade step of 256 (0x0800AE48,
        // 0x08000BCE, 0x0800AC64), and moves it to order 54 (0x0800AC70, 0x08003290, 0x0800FCF4, 0x0801664C,
        // 0x08016696), 25 (0x0800DCD8, 0x0801323A, 0x080132C2), and to the order of a table of 8 by the table's number
        // (0x0832A8C8): 177, 99, 65, 29, 86, 156, 195 and 120. A move sets the step to 256.
        {"BHOP",
         0,
         {{0x080393CD, {0x030031D4, 0x03004060, 0x03007C00 - 0x03004060}},
          {0x08035B89, {8, 0x46ED, 304, 0x50}},
          {0x08035C01, {}}},
         0x08039261,
         {},
         {{0x0832A954, Starts({0, 25, 29, 54, 65, 86, 99, 120, 156, 177, 195}, kFullVolume)}}},
        // Garfield: The Search for Pooky. The main routine makes the heap in IWRAM (0x0802B5D8: 0x0802D774 with the
        // heap's word 0x030014B0, 0x0300233C and the bytes up to 0x03007C00). The game's init sets up the mixer
        // (0x0800038C: 0x08029D50 with 8 channels, 304 samples a frame and master volume 0x30) and the sound's
        // registers (0x08029DC8). Every start goes through 0x0802D6C8. That routine sets the step to 256 and starts the
        // module with 256 (0x0802D5CC). Its calls: module 0x08766D14 at 0 (0x0800195C, 0x080034D4, 0x0800A130,
        // 0x0800C114, 0x0800D73E), 80 (0x08017A52, 0x08017E04, 0x08018148), 177 (0x08001C2A), 187 (0x08001B12), 191
        // (0x08004514, 0x0800BB6E, 0x0800DD70) and 199 (0x0800D62C); module 0x087BFDD4 at 28 (0x0800D25E), 132
        // (0x08003352) and 138 (0x08009E26). A level's set-up (0x080165E8) takes a case from the table of rooms at
        // 0x08594640 and jumps through the table at 0x08597488. These cases load a module and an order, then call the
        // start at 0x080168D6: module 0x08766D14 at 22 (cases 35 to 37), 41 (1 to 3), 112 (38 to 40), 138 (41 to 43)
        // and 217 (32 to 34), and module 0x087BFDD4 at 0 (16 to 18), 49 (13 to 15), 83 (4 to 6) and 117 (10 to 12).
        // Cases 7 to 9 start module 0x08766D14 at 80 through the calls at 0x08017A52, 0x08017E04 and 0x08018148. Order
        // 49 starts the part of the song from 28 at its loop point.
        {"BGOE",
         0,
         {{0x0802D775, {0x030014B0, 0x0300233C, 0x03007C00 - 0x0300233C}},
          {0x08029D51, {8, 0x46ED, 304, 0x30}},
          {0x08029DC9, {}}},
         0x0802D5CD,
         {.master = 0x30},
         {{0x08766D14, Starts({0, 22, 41, 80, 112, 138, 177, 187, 191, 199, 217}, kFullVolume)},
          {0x087BFDD4, Starts({0, 28, 49, 83, 117, 132, 138}, kFullVolume)}}},
        // Games Explosion!. The game's ARM code sets up the mixer (0x0803D344: 0x0800055C with 8 channels, 304 samples
        // a frame and master volume 0x40) and the sound's registers (0x0803D1D8, ARM). The mixer takes its channels
        // from a fixed place. The game starts the module through 0x0803D2AC, or 0x08000218 at once, with a fade step of
        // 31: order 0 (0x08002B54 through 0x08000ACC), 28 (0x0800F448, 0x0802CC24), 40 (0x0802E2B0), 75 (0x080297F8,
        // 0x0803CF88), 99 (0x08022A60, 0x08026660), 112 (0x0800A774, 0x08011C54, 0x080139BC, 0x08032344), 138
        // (0x0801BC1C, 0x0801E674), 159 (0x0802A8FC, 0x0802C7B4) and 176 (0x080357B4, 0x08037F98). It starts 199, 219
        // and 230 with a step of 255 (0x08010CA8, 0x08010C70, 0x08010C8C).
        {"BG7E",
         0,
         {{0x0800055D, {8, 0x46ED, 304, 0x40}}, {0x0803D1D8, {}}},
         0x08000219,
         {.master = 0x40},
         {{0x082BF4B0,
           []
           {
               std::vector<GameStart> starts = Starts({0, 28, 40, 75, 99, 112, 138, 159, 176}, 31);
               const std::vector<GameStart> late = Starts({199, 219, 230}, 255);
               starts.insert(starts.end(), late.begin(), late.end());
               return starts;
           }()}},
         31},
    };
    return kGames;
}

// A release whose image equals another's byte for byte outside the cartridge's header. It takes that release's set-up
// and songs.
struct Alias
{
    const char* code;
    uint8_t version;
    const char* game_code;
    uint8_t game_version;
};

constexpr Alias kAliases[] = {
    // Garfield: The Search for Pooky, Europe, revision 2: the US release.
    {"BGOP", 2, "BGOE", 0},
};

// Returns the rows that the player reads from a pattern of `rows`. A pattern of 0 rows plays one: the player reads a
// row before it compares the next row with the count.
uint32_t PlayedRows(uint32_t rows)
{
    return rows == 0 ? 1 : rows;
}

bool InIwram(uint32_t address)
{
    return address >= kIwramBase && address < kIwramBase + kIwramSize;
}

// Finds the start-up code's words of the copy that covers `base`'s image: the source, the destination in IWRAM and
// the end.
bool FindCopy(const Rom& rom, uint32_t base, DriverInfo& info)
{
    for (uint32_t at = kRomBase; at + 12 <= kRomBase + kStartupWords && rom.Contains(at, 12); at += 4)
    {
        const uint32_t source = rom.U32(at);
        const uint32_t destination = rom.U32(at + 4);
        const uint32_t end = rom.U32(at + 8);
        if (InIwram(destination) && InIwram(end) && end > destination && source == base + (destination - kIwramBase))
        {
            info.copy_start = destination;
            info.copy_end = end;
            return true;
        }
    }

    return false;
}

// Finds the player and the mixer, and reads their pools.
bool FindCode(const Rom& rom, DriverInfo& info)
{
    const uint32_t player = agb::FindArm(rom, kRomBase, rom.End(), kPlayerCode);
    if (player == 0)
    {
        return false;
    }

    info.effects = rom.U32(player + kPlayerEffects);
    info.state = rom.U32(player + kPlayerState);
    info.context = rom.U32(player + kPlayerContext);
    info.periods = rom.U32(player + kPlayerPeriods);
    info.vibrato = rom.U32(player + kPlayerVibrato);
    info.player = info.effects - kEffectsOffset;
    info.image_base = player - (info.player - kIwramBase);

    const uint32_t mixer = agb::FindArm(rom, info.image_base, rom.End(), kMixerCode);
    if (mixer == 0)
    {
        return false;
    }

    info.mixer = kIwramBase + (mixer - info.image_base);
    info.clip_table = rom.U32(mixer + kMixerClipTable);
    info.output = rom.U32(mixer + kMixerOutput);
    info.accumulator = rom.U32(mixer + kMixerAccumulator);
    return true;
}

// Reads a cell from `at`. Returns the address after it.
uint32_t SkipCell(const Rom& rom, uint32_t at, uint8_t& flags, uint8_t& parameter)
{
    flags = rom.U8(at++);
    parameter = 0;
    if (flags & kCellNote)
    {
        at += 3;
    }

    if (flags & kCellVolume)
    {
        at++;
    }

    if (flags & kCellEffect)
    {
        parameter = rom.U8(at++);
    }

    return at;
}

// Returns true if a cell of order `order` of `module` meets `test`. `test` takes the cell's flags and parameter.
template <class Test>
bool AnyCell(const Rom& rom, const Module& module, uint32_t order, Test test)
{
    const uint32_t pattern = rom.U32(module.orders + 4 * order);
    const uint32_t rows = PlayedRows(rom.U32(pattern + kPatternRows));
    uint32_t at = rom.U32(pattern + kPatternCells);
    for (uint32_t i = 0; i < rows * module.channels; i++)
    {
        uint8_t flags = 0, parameter = 0;
        at = SkipCell(rom, at, flags, parameter);
        if (test(flags, parameter))
        {
            return true;
        }
    }

    return false;
}

// Returns true if order `order` of `module` contains a jump, and sets `target` to its order.
bool OrderJumps(const Rom& rom, const Module& module, uint32_t order, uint32_t& target)
{
    const auto jumps = [&](uint8_t flags, uint8_t parameter)
    {
        if ((flags & kCellEffect) != 4 * kEffectJump)
        {
            return false;
        }

        target = parameter;
        return true;
    };

    return AnyCell(rom, module, order, jumps);
}

// Returns true if order `order` of `module` starts a note.
bool OrderHasNotes(const Rom& rom, const Module& module, uint32_t order)
{
    const auto starts_note = [](uint8_t flags, uint8_t)
    {
        return (flags & kCellNote) && flags != kCellSlideTarget;
    };

    return AnyCell(rom, module, order, starts_note);
}

// Adds to `reached` the orders of `module` that a song from `order` plays: each order's pattern, then the order that
// its jump names, or the next order, until an order comes back.
void AddReachedOrders(const Rom& rom, const Module& module, uint32_t order, std::set<uint32_t>& reached)
{
    std::set<uint32_t> played;
    while (order < module.order_count && played.insert(order).second)
    {
        reached.insert(order);
        uint32_t target = 0;
        order = OrderJumps(rom, module, order, target) ? target : order + 1 < module.order_count ? order + 1 : 0;
    }
}

// Adds the songs of the parts of `module` that no song plays: the orders from the one after a jump up to the next
// jump, or to the last order, where no order is one that a start of the game, or a song added before, reaches.
void AddUnstartedSongs(const Rom& rom, int index, const Module& module, int32_t step, std::vector<Song>& songs)
{
    std::set<uint32_t> reached;
    for (const Song& song : songs)
    {
        if (song.module == index)
        {
            AddReachedOrders(rom, module, song.order, reached);
        }
    }

    std::vector<Song> added;
    uint32_t first = 0;
    while (first < module.order_count)
    {
        uint32_t last = first, target = 0;
        while (last + 1 < module.order_count && !OrderJumps(rom, module, last, target))
        {
            last++;
        }

        const auto covered = reached.lower_bound(first);
        bool notes = false;
        for (uint32_t order = first; order <= last && !notes; order++)
        {
            notes = OrderHasNotes(rom, module, order);
        }

        if ((covered == reached.end() || *covered > last) && notes)
        {
            added.push_back({index, first, step, true});
            AddReachedOrders(rom, module, first, reached);
        }

        first = last + 1;
    }

    songs.insert(songs.end(), added.begin(), added.end());
}

// Returns "song N" or "songs N-M" for the songs from `first` up to `end`.
std::string SongRange(size_t first, size_t end)
{
    return end - first == 1 ? "song " + std::to_string(first)
                            : "songs " + std::to_string(first) + "-" + std::to_string(end - 1);
}

// Returns "order N" or "orders A, B and C".
std::string OrderList(const std::vector<uint32_t>& orders)
{
    std::string text = orders.size() == 1 ? "order " : "orders ";
    for (size_t k = 0; k < orders.size(); k++)
    {
        text += (k == 0 ? "" : k + 1 == orders.size() ? " and " : ", ") + std::to_string(orders[k]);
    }

    return text;
}

} // namespace

uint32_t DriverInfo::ImageAddress(uint32_t address) const
{
    if (address < copy_start || address >= copy_end)
    {
        return 0;
    }

    return image_base + (address - kIwramBase);
}

uint32_t MixerSettings::ClipSpan() const
{
    // The builder takes the low 7 bits of its parameter, and 16 at the least.
    const uint32_t scale = std::max<uint32_t>(clip_scale & 0x7F, kMinClipScale);
    return 0x8000 / scale;
}

double DriverInfo::Rate() const
{
    return double(1u << 24) / timer_cycles;
}

bool ReadModule(const Rom& rom, uint32_t address, Module& module)
{
    if (!rom.Contains(address, kHeaderSize))
    {
        return false;
    }

    module.address = address;
    module.channels = rom.U32(address + kHeaderChannels);
    module.order_count = rom.U32(address + kHeaderOrderCount);
    module.pattern_count = rom.U32(address + kHeaderPatternCount);
    module.speed = rom.U32(address + kHeaderSpeed);
    module.instruments = rom.U32(address + kHeaderInstruments);
    module.orders = rom.U32(address + kHeaderOrders);
    if (module.channels < 1 || module.channels > kMaxChannels || module.order_count < 1 ||
        module.order_count > kMaxOrders || module.pattern_count < 1 || module.pattern_count > kMaxPatterns ||
        module.speed > kMaxSpeed || !rom.Contains(module.instruments, 4) ||
        !rom.Contains(module.orders, 4 * module.order_count))
    {
        return false;
    }

    // Every order's pattern reads to its last row.
    for (uint32_t order = 0; order < module.order_count; order++)
    {
        const uint32_t pattern = rom.U32(module.orders + 4 * order);
        if (!rom.Contains(pattern, 8))
        {
            return false;
        }

        const uint32_t rows = PlayedRows(rom.U32(pattern + kPatternRows));
        uint32_t at = rom.U32(pattern + kPatternCells);
        if (rows > kMaxRows || !rom.Contains(at))
        {
            return false;
        }

        for (uint32_t i = 0; i < rows * module.channels; i++)
        {
            uint8_t flags = 0, parameter = 0;
            at = SkipCell(rom, at, flags, parameter);
            if (!rom.Contains(at - 1))
            {
                return false;
            }
        }
    }

    return true;
}

int FindJump(const Rom& rom, const Module& module, uint32_t order, uint32_t& target)
{
    for (uint32_t at = order; at < module.order_count; at++)
    {
        if (OrderJumps(rom, module, at, target))
        {
            return int(at);
        }
    }

    return -1;
}

bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    if (!FindCode(rom, info))
    {
        return false;
    }

    if (!FindCopy(rom, info.image_base, info))
    {
        error = "Frontline Studios' MOD player: the start-up code's copy of the player to IWRAM wasn't found";
        return false;
    }

    info.log.push_back("player " + Hex(info.player) + ", mixer " + Hex(info.mixer) + ", IWRAM image at " +
                       Hex(info.image_base + (info.copy_start - kIwramBase)) + " for " + Hex(info.copy_start) + " to " +
                       Hex(info.copy_end));

    // The game's set-up and songs.
    std::string code = rom.GameCode();
    uint8_t version = rom.Version();
    for (const Alias& alias : kAliases)
    {
        if (code == alias.code && version == alias.version)
        {
            code = alias.game_code;
            version = alias.game_version;
        }
    }

    const auto game = std::find_if(Games().begin(), Games().end(),
                                   [&](const Game& g) { return code == g.code && version == g.version; });
    if (game == Games().end())
    {
        error = "Frontline Studios' MOD player: the game's set-up of the player isn't known for game code " + code;
        return false;
    }

    info.game_code = code;
    info.game_version = version;
    info.init = game->init;
    info.start = game->start;
    info.mixer_settings = game->mixer;
    info.timer_cycles = kFrameCycles / info.mixer_settings.samples;
    for (const GameModule& m : game->modules)
    {
        Module module;
        if (!ReadModule(rom, m.address, module))
        {
            error = "Frontline Studios' MOD player: the module at " + Hex(m.address) + " can't be read";
            return false;
        }

        // The module's songs: the game's starts, then the parts that no start reaches.
        const int index = int(info.modules.size());
        const size_t first = info.songs.size();
        info.modules.push_back(module);
        std::vector<uint32_t> started;
        for (const GameStart& start : m.starts)
        {
            info.songs.push_back({index, start.order, start.step, false});
            started.push_back(start.order);
        }

        AddUnstartedSongs(rom, index, module, game->unstarted_step, info.songs);

        std::vector<uint32_t> unstarted;
        for (size_t s = first + started.size(); s < info.songs.size(); s++)
        {
            unstarted.push_back(info.songs[s].order);
        }

        std::string line = SongRange(first, info.songs.size()) + ": module " + Hex(module.address) + ", " +
                           std::to_string(module.channels) + " channels, " + std::to_string(module.order_count) +
                           " orders; the game starts " + OrderList(started);
        if (!unstarted.empty())
        {
            line += "; " + SongRange(first + started.size(), info.songs.size()) +
                    " start after the module's jumps, at " + OrderList(unstarted);
        }

        info.log.push_back(line);
    }

    return true;
}

} // namespace agblib::frontline_mod
