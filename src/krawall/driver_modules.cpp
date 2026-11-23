// SPDX-License-Identifier: MIT

// Krawall's module format: the header of a module, its samples, the rows of its patterns and their entries.

#include <algorithm>
#include <cstdint>
#include <initializer_list>

#include "agblib/agb/arm.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/thumb.h"
#include "agblib/krawall/driver.h"
#include "driver_internal.h"

namespace agblib::krawall
{
namespace
{

// Returns true if a pattern's header is one the player can read: a row count, and an index of every 4th row's data that
// starts at the data and doesn't go back.
bool PlausiblePattern(const Rom& rom, Build build, uint32_t at)
{
    const uint32_t data = PatternData(build, at);
    if (!rom.Contains(at, data - at))
    {
        return false;
    }

    const uint32_t rows = PatternRows(rom, build, at);
    if (rows == 0 || rom.U16(at) != 0)
    {
        return false;
    }

    uint16_t last = 0;
    for (uint32_t i = 1; i < kPatternIndexEntries && 4 * i < rows; i++)
    {
        const uint16_t offset = rom.U16(at + 2 * i);
        if (offset < last || !rom.Contains(data + offset))
        {
            return false;
        }

        last = offset;
    }

    return true;
}

} // namespace

bool ReadModule(const Rom& rom, Build build, uint32_t address, ModuleInfo& module)
{
    module = ModuleInfo();
    if (!rom.Contains(address, kModulePatterns))
    {
        return false;
    }

    module.address = address;
    module.channels = rom.U8(address + kModuleChannels);
    module.order_count = rom.U8(address + kModuleOrderCount);
    module.restart = rom.U8(address + kModuleRestart);
    for (uint32_t i = 0; i < 256; i++)
    {
        module.orders[i] = rom.U8(address + kModuleOrders + i);
    }

    for (uint32_t i = 0; i < 32; i++)
    {
        module.channel_pan[i] = rom.S8(address + kModuleChannelPans + i);
    }

    for (uint32_t i = 0; i < 64; i++)
    {
        module.song_starts[i] = rom.U8(address + kModuleSongStarts + i);
    }

    module.global_volume = rom.U8(address + kModuleGlobalVolume);
    module.speed = rom.U8(address + kModuleSpeed);
    module.tempo = rom.U8(address + kModuleTempo);
    const uint8_t flags[3] = {rom.U8(address + kModuleFlags), rom.U8(address + kModuleFlags + 1),
                              rom.U8(address + kModuleFlags + 2)};
    module.instruments = flags[0] != 0;
    module.linear = flags[1] != 0;
    module.fast_slides = flags[2] != 0;
    module.volume_opt = rom.U8(address + kModuleFlags + 3);
    module.amiga_limits = rom.U8(address + kModuleFlags + 4);

    // The header's flags are bytes of 0 or 1, and the pans are within the player's range.
    if (module.channels == 0 || module.channels > 32 || module.order_count == 0 || module.speed == 0)
    {
        return false;
    }

    for (uint8_t f : {flags[0], flags[1], flags[2], module.volume_opt, module.amiga_limits})
    {
        if (f > 1)
        {
            return false;
        }
    }

    for (int c = 0; c < module.channels; c++)
    {
        if (module.channel_pan[size_t(c)] < -64 || module.channel_pan[size_t(c)] > 64)
        {
            return false;
        }
    }

    // The orders name the patterns. Their table follows the header. In the modified build, the address of the
    // module's instruments comes before it.
    const uint32_t table = address + ModulePatterns(build);
    if (build == Build::kModified)
    {
        module.instruments_at = rom.U32(address + kModuleInstruments);
    }

    int patterns = 0;
    for (int i = 0; i < module.order_count; i++)
    {
        const uint8_t o = module.orders[size_t(i)];
        if (o < kOrderSkip)
        {
            patterns = std::max(patterns, o + 1);
        }
    }

    if (patterns == 0 || !rom.Contains(table, 4 * uint32_t(patterns)))
    {
        return false;
    }

    for (int p = 0; p < patterns; p++)
    {
        const uint32_t pattern = rom.U32(table + 4 * uint32_t(p));
        if (!PlausiblePattern(rom, build, pattern))
        {
            return false;
        }

        module.patterns.push_back(pattern);
    }

    return true;
}

uint32_t SampleEnd(const Rom& rom, Build build, uint32_t sample)
{
    // The modified build's header gives the points' count where the others give the address after them.
    const uint32_t end = rom.U32(sample + kSampleEnd);
    return build == Build::kModified ? sample + kSamplePoints + end : end;
}

uint32_t ModulePatterns(Build build)
{
    return build == Build::kModified ? kModifiedPatterns : kModulePatterns;
}

const char* BuildName(Build build)
{
    switch (build)
    {
    case Build::kV1_2_2_10:
        return "player.c 1.2.2.10 of 2002/08/24";
    case Build::kV1_2_2_14:
        return "player.c 1.2.2.14 of 2002/09/12";
    case Build::kV1_2_2_15:
        return "player.c 1.2.2.15 of 2002/12/19";
    case Build::kV1_2_2_17:
        return "player.c 1.2.2.17 of 2003/01/13";
    case Build::kV1_4:
        return "player.c 1.4 of 2003/03/19";
    case Build::k2003_09_01:
        return "2003/09/01";
    case Build::k2004_09_17:
        return "2004/09/17";
    case Build::k2004_11_14:
        return "2004/11/14";
    case Build::k2005_04_21:
        return "2005-04-21";
    case Build::kUnsigned:
        return "unsigned";
    case Build::kModified:
        return "modified";
    }

    return "unknown";
}

uint32_t PatternRows(const Rom& rom, Build build, uint32_t pattern)
{
    return build < Build::k2004_09_17 ? rom.U8(pattern + kPatternRows) : rom.U16(pattern + kPatternRows);
}

uint32_t PatternData(Build build, uint32_t pattern)
{
    return pattern + kPatternRows + (build < Build::k2004_09_17 ? 1 : 2);
}

RowEntry ReadEntry(const Rom& rom, Build build, uint8_t flags, uint32_t& at)
{
    // Up to 2003/09/01, the note is in the first byte's top 7 bits. The second byte contains the instrument's low 8
    // bits, and the first byte's low bit contains its 9th. From 2004/09/17 on, the note is in the first byte's low 7
    // bits, the second byte contains the instrument's low 8 bits, and where the first byte's top bit is set, a third
    // byte contains its top 8.
    RowEntry entry;
    entry.channel = flags & kEntryChannel;
    if ((flags & kEntryNote) && build < Build::k2004_09_17)
    {
        const uint8_t b0 = rom.U8(at++);
        entry.instrument = uint16_t(rom.U8(at++) | ((b0 & 1) << 8));
        entry.note = uint8_t(b0 >> 1);
    }
    else if (flags & kEntryNote)
    {
        const uint8_t b0 = rom.U8(at++);
        entry.instrument = rom.U8(at++);
        if (b0 & 0x80)
        {
            entry.instrument = uint16_t(entry.instrument | (rom.U8(at++) << 8));
        }

        entry.note = uint8_t(b0 & 0x7F);
    }

    if (flags & kEntryVolume)
    {
        entry.volume = rom.U8(at++);
    }

    if (flags & kEntryEffect)
    {
        entry.effect = rom.U8(at++);
        entry.effect_op = rom.U8(at++);
    }

    return entry;
}

} // namespace agblib::krawall
