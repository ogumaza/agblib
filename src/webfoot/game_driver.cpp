// SPDX-License-Identifier: MIT

#include "agblib/webfoot/game_driver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/arm/cpu.h"

#include "agblib/agb/arm.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/agb/thumb.h"
#include "agblib/webfoot/driver.h"

namespace agblib::webfoot
{

using agblib::agb::Hex;
using agblib::agb::kIwramBase;
using agblib::agb::kRomBase;

namespace
{

// The registers that the handler and the init use.
constexpr uint32_t kRegDma1Source = 0x040000BC;
constexpr uint32_t kRegDma2Source = 0x040000C8;
constexpr uint32_t kRegInterruptEnable = 0x04000200;

// The play routine's code in each build: 0x0800A5F6 in Dragon Ball Z: The Legacy of Goku (2002), 0x08020A4C in The
// Legacy of Goku II (later). Detection finds the init and the setter of the music volume (FindInit(),
// FindMusicVolume()).
constexpr std::string_view k2002Play = "B570 4Cxx 2601 70A6 1C05 2000 70E6 7060 60E5";
constexpr std::string_view k2003Play = "B570 2501 7085 7005 2200 7042 60C1 1C04 7C48 70E0 43D0 6120 2000";

// The init's store of the handler into the game's table of interrupt handlers. The 2002 build:
// `ldr r2, =handler; ldr r3, =slot; ldr r6, =0x04000200; str r2, [r3]` (0x0800AAA6). The later build:
// `ldr rA, =table; ldr rB, =handler; ldr rA, [rA]; str rB, [rA, #slot]` (0x08020B5C in The Legacy of Goku II), in
// other orders in the other programs (Find2003HandlerLoad).
constexpr std::string_view k2002HandlerStore = "4Axx 4Bxx 4Exx 601A";

// The handler's add to the FIFOs' place: `adds r0, #n` (Thumb, 2002) and `add r1, r1, #n` (ARM, later).
constexpr uint32_t k2002AddOffset = 4;
constexpr uint16_t k2002Add = 0x3000;
constexpr uint32_t k2003AddOffset = 8;
constexpr uint32_t k2003Add = 0xE2811000;

// The driver object's fields in the later build: the FIFOs' place and the music player.
constexpr uint32_t kObjectPositions = 4;
constexpr uint32_t kObjectPlayer = 0xC;

// The registers that the handlers return through.
constexpr int k2002Return = 6;
constexpr int k2003Return = 4;

// Returns the first match of `pattern` at or after `from`, within kRoutineReach of it when `near` is set, or 0.
uint32_t FindFrom(const Rom& rom, std::string_view pattern, uint32_t from, bool near = false)
{
    for (const uint32_t at : agb::FindThumb(rom, agb::ParseThumbPattern(pattern)))
    {
        if (at >= from && (!near || at < from + kRoutineReach))
        {
            return at;
        }
    }

    return 0;
}

// Returns the address of the later build's load of the handler in the init at `init`, or 0. The init stores the
// handler into the game's table of interrupt handlers, whose address it reads from the ROM: `ldr rA, [rA]`, then within
// two instructions `str rB, [rA, #slot]`. rB comes from a literal a few instructions before.
uint32_t Find2003HandlerLoad(const Rom& rom, uint32_t init)
{
    constexpr uint32_t kLoadReach = 16;
    for (uint32_t at = init; at < init + kRoutineReach; at += 2)
    {
        const uint16_t table = rom.U16(at);
        const uint32_t a = table & 7u;
        if ((table & 0xFFC0) != 0x6800 || ((table >> 3) & 7u) != a)
        {
            continue;
        }

        for (uint32_t store = at + 2; store <= at + 4; store += 2)
        {
            const uint16_t s = rom.U16(store);
            if ((s & 0xF800) != 0x6000 || ((s >> 3) & 7u) != a)
            {
                continue;
            }

            const uint32_t b = s & 7u;
            for (uint32_t load = at - 2; load + kLoadReach >= at && load >= init; load -= 2)
            {
                if ((rom.U16(load) & 0xFF00) == (0x4800 | b << 8))
                {
                    return load;
                }
            }
        }
    }

    return 0;
}

bool Find2002(const Rom& rom, GameRoutines& r, std::string& error)
{
    r.play = FindFrom(rom, k2002Play, r.program);
    r.music_volume = FindMusicVolume(rom, Build::k2002, r.program);
    r.init = FindInit(rom, Build::k2002, r.program);
    if (r.play == 0 || r.music_volume == 0 || r.init == 0)
    {
        error = "found play at " + Hex(r.play) + ", the music volume at " + Hex(r.music_volume) + " and the init at " +
                Hex(r.init) + "; 0 is a routine that isn't there";
        return false;
    }

    r.init_argument = FindInitRate(rom, r.init);
    const uint32_t store = FindFrom(rom, k2002HandlerStore, r.init, true);
    if (r.init_argument == 0 || store == 0)
    {
        error = "the 2002 build's call of the init or the init's store of its handler isn't there";
        return false;
    }

    r.handler = agb::ThumbLiteral(rom, store);
    const uint32_t code = r.handler & ~1u;
    r.positions = agb::ThumbLiteral(rom, code);
    const uint16_t add = rom.U16(code + k2002AddOffset);
    r.samples_per_interrupt = (add & 0xFF00) == k2002Add ? add & 0xFFu : 0;
    r.return_register = k2002Return;

    return true;
}

bool Find2003(const Rom& rom, GameRoutines& r, std::string& error)
{
    // The program's crt0 copies the image of IWRAM that contains the handler and the mixer.
    if (!ReadIwramImage(rom, r.program, r.iwram_source, r.iwram_size))
    {
        error = "the crt0 of the program at " + Hex(r.program) + " copies no image of IWRAM";
        return false;
    }

    r.play = FindFrom(rom, k2003Play, r.program);
    r.music_volume = FindMusicVolume(rom, Build::k2003, r.program);
    r.init = FindInit(rom, Build::k2003, r.program);
    const uint32_t load = r.init ? Find2003HandlerLoad(rom, r.init) : 0;
    if (r.play == 0 || r.music_volume == 0 || r.init == 0 || load == 0)
    {
        error = "found play at " + Hex(r.play) + ", the music volume at " + Hex(r.music_volume) + ", the init at " +
                Hex(r.init) + " and its load of the handler at " + Hex(load) + "; 0 is a routine that isn't there";
        return false;
    }

    // The handler is ARM code in IWRAM. Its first literal is the driver's object.
    r.handler = agb::ThumbLiteral(rom, load);
    const uint32_t code = r.iwram_source + (r.handler - kIwramBase);
    if (r.handler < kIwramBase || r.handler + 12 > kIwramBase + r.iwram_size)
    {
        error = "the handler " + Hex(r.handler) + " is outside the image of IWRAM";
        return false;
    }

    r.init_argument = agb::ArmLiteral(rom, code);
    r.player = r.init_argument + kObjectPlayer;
    r.positions = r.init_argument + kObjectPositions;
    const uint32_t add = rom.U32(code + k2003AddOffset);
    r.samples_per_interrupt = (add & 0xFFFFFF00) == k2003Add ? add & 0xFFu : 0;
    r.return_register = k2003Return;

    return true;
}

} // namespace

bool FindGameRoutines(const Rom& rom, const SongTable& table, GameRoutines& routines, std::string& error)
{
    routines = {};
    routines.build = table.build;
    routines.program = table.program;
    const bool found = table.build == Build::k2002 ? Find2002(rom, routines, error) : Find2003(rom, routines, error);
    if (found && routines.samples_per_interrupt == 0)
    {
        error = "the handler at " + Hex(routines.handler) + " doesn't count its samples as the driver does";
        return false;
    }

    if (found)
    {
        routines.master_handler = FindMasterHandler(rom, routines);
    }

    return found;
}

uint32_t FindMasterHandler(const Rom& rom, const GameRoutines& routines)
{
    const bool iwram = routines.iwram_size != 0;
    const uint32_t start = iwram ? routines.iwram_source : routines.program;
    const uint32_t end = iwram ? routines.iwram_source + routines.iwram_size : routines.program + kMasterHandlerReach;
    for (uint32_t at = start; at + 4 * kMasterHandlerCode.size() <= end; at += 4)
    {
        bool same = true;
        for (size_t i = 0; i < kMasterHandlerCode.size() && same; i++)
        {
            same = rom.U32(at + 4 * uint32_t(i)) == kMasterHandlerCode[i];
        }

        if (same)
        {
            return iwram ? kIwramBase + (at - routines.iwram_source) : at;
        }
    }

    return 0;
}

void CallHandler(agb::System& gba, const GameRoutines& routines)
{
    gba.Cpu().SetReg(routines.return_register, ntdlib::arm::Cpu::kCallReturn);
    gba.Call(routines.handler, std::array<uint32_t, 4>{0, 0, 0, kRegInterruptEnable});
}

GameDriver::GameDriver(const Rom& rom, const GameRoutines& routines) : routines_(routines), system_(rom)
{
    for (uint32_t i = 0; i < routines_.iwram_size; i++)
    {
        system_.Store(kIwramBase + i, 1, rom.U8(routines_.iwram_source + i));
    }

    system_.Call(routines_.init | 1, std::array<uint32_t, 1>{routines_.init_argument});

    // The init points the DMAs at the start of the left ring and at the middle of the right one.
    left_ring_ = system_.Load(kRegDma1Source, 4);
    right_ring_ = system_.Load(kRegDma2Source, 4) - kRightRestart;
}

void GameDriver::SetMusicVolume(int volume)
{
    if (routines_.build == Build::k2002)
    {
        system_.Call(routines_.music_volume | 1, std::array<uint32_t, 1>{uint32_t(volume)});
    }
    else
    {
        system_.Call(routines_.music_volume | 1, std::array<uint32_t, 2>{routines_.player, uint32_t(volume)});
    }
}

void GameDriver::Play(uint32_t header)
{
    if (routines_.build == Build::k2002)
    {
        system_.Call(routines_.play | 1, std::array<uint32_t, 1>{header});
    }
    else
    {
        system_.Call(routines_.play | 1, std::array<uint32_t, 2>{routines_.player, header});
    }
}

uint32_t GameDriver::PlayPlace()
{
    return system_.Load(routines_.positions, 4);
}

uint32_t GameDriver::MixPlace()
{
    return system_.Load(routines_.positions + 4, 4);
}

bool GameDriver::Interrupt()
{
    output_.clear();
    const uint32_t before = MixPlace();
    CallHandler(system_, routines_);
    const uint32_t after = MixPlace();
    if (after == before)
    {
        return false;
    }

    const uint32_t count = (after - before) & (kRingSize - 1);
    output_.reserve(2 * count);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t p = (before + i) & (kRingSize - 1);
        output_.push_back(int8_t(system_.Load(left_ring_ + p, 1)));
        output_.push_back(int8_t(system_.Load(right_ring_ + ((p + kRightRestart) & (kRingSize - 1)), 1)));
    }

    return true;
}

const std::vector<int8_t>& GameDriver::Output() const
{
    return output_;
}

uint16_t GameDriver::TimerReload()
{
    return uint16_t(system_.Load(agb::kRegTimer0Reload, 2));
}

uint16_t GameDriver::InterruptsEnabled()
{
    return uint16_t(system_.Load(kRegInterruptEnable, 2));
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace agblib::webfoot
