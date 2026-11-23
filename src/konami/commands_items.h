// SPDX-License-Identifier: MIT

// The items of Konami's commands: their meaning, their units and their bytes. Only commands.cpp includes this header.
// Its definitions therefore keep internal linkage.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/konami/commands.h"

namespace agblib::konami
{
namespace
{

using agb::kRomBase;
using Item = ntdlib::commands::Command;
using ntdlib::commands::FromUnit;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::ToUnit;

// The first sample track, for the commands that read otherwise on a sample track.
constexpr int kSampleTrack = kPsgTracks;

// The bytes that the longest command or delay takes up.
constexpr std::size_t kLongestItem = 8;

// Whether revision `r`'s tracks keep the return position after a call's last command, and go back there every 256
// commands after it.
bool KeepsReturn(Revision r)
{
    return r == Revision::kEternalDuelist || r == Revision::kDungeonDiceMonsters;
}

// Returns command `c` as an item of the common form, in the console's values. It contains the arguments that a command
// of kind kOther or kLoopStart keeps. A call's target and the kept bytes stay out.
Item RawItem(const Rom& rom, const konami::Command& c)
{
    Item item;
    const auto set = [&item](Kind kind, int64_t value = 0, uint8_t index = 0)
    {
        item.kind = kind;
        item.value.value = value;
        item.index = index;
    };

    const auto rest = [&]
    {
        for (uint32_t i = 1; i < c.length; i++)
        {
            item.bytes.push_back(rom.U8(c.addr + i));
        }
    };

    switch (c.op)
    {
    case Op::kDelay:
        set(Kind::kWait, c.delay);
        break;

    case Op::kDuty:
        set(Kind::kProgram, c.value, 0);
        break;

    case Op::kWave:
        set(Kind::kProgram, c.value, 1);
        break;

    case Op::kInstrument:
        set(Kind::kProgram, c.value, 2);
        break;

    case Op::kNote:
    case Op::kNextNote:
        set(Kind::kNote, 0, c.op == Op::kNextNote);
        item.sample = uint16_t(c.sample);
        item.key = uint8_t(c.semitone);
        item.velocity = c.vol;
        break;

    case Op::kPsgNote:
    case Op::kSampleAtNote:
        set(Kind::kNote, 0, c.op == Op::kSampleAtNote);
        item.key = uint8_t(c.note);
        item.velocity = c.vol;
        break;

    case Op::kVolume:
        set(Kind::kVolume, c.vol, 0);
        break;

    case Op::kSetVolume:
        set(Kind::kVolume, c.vol, 1);
        break;

    case Op::kPairVolumes:
        set(Kind::kVolume, c.value & 15, 2);
        item.velocity = c.value >> 4;
        break;

    case Op::kRest:
        set(Kind::kNoteOff);
        break;

    case Op::kRelease:
        set(Kind::kRelease, 0, 0);
        break;

    case Op::kFade:
        set(Kind::kRelease, 0, c.opcode & 1 ? 2 : 1);
        break;

    case Op::kPan:
        set(Kind::kPan, c.value);
        break;

    case Op::kPanLevels:
        set(Kind::kPan, (c.value & 15) - (c.value >> 4));
        item.velocity = std::max(c.value & 15, c.value >> 4);
        break;

    case Op::kVibrato:
        set(Kind::kLfoDepth, c.value);
        break;

    case Op::kPitchBend:
        set(Kind::kPitchBend, c.value, 0);
        break;

    case Op::kSampleBend:
        set(Kind::kPitchBend, c.value, c.opcode & 1 ? 2 : 1);
        break;

    case Op::kLoopPoint:
        set(Kind::kLoopStart);
        rest();
        break;

    case Op::kEchoFeedback:
        set(Kind::kEchoVolume, c.value);
        break;

    case Op::kEchoDelay:
        set(Kind::kEchoLength, c.value);
        break;

    case Op::kEndTrack:
        set(Kind::kEnd, 0, 0);
        break;

    case Op::kEndTrackHard:
        set(Kind::kEnd, 0, 1);
        break;

    case Op::kJump:
        if (c.value != 0)
        {
            set(Kind::kLoopEnd);
        }
        else
        {
            set(Kind::kEnd, 0, 2);
        }
        break;

    case Op::kAttack:
        set(Kind::kAttack, c.value);
        break;

    case Op::kDecay:
        set(Kind::kDecay, c.value);
        break;

    case Op::kHoldOutput:
        set(Kind::kMute, c.value);
        break;

    case Op::kSongVolume:
        set(Kind::kMainVolume, c.value);
        break;

    case Op::kCall:
        set(Kind::kCall, c.count);
        item.key = c.opcode;
        break;

    case Op::kNop:
    case Op::kEchoRoute:
    case Op::kVolumeScale:
    case Op::kPsgPan:
    case Op::kPsgVolume:
    case Op::kUnknown:
        set(Kind::kOther);
        item.encoding.opcode = c.opcode;
        rest();
        break;
    }

    return item;
}

// Whether two items in the console's values mean the same. A call's target and the kept bytes don't count. Each kind's
// arguments do: the bytes of a loop point or of a command of kOther are its meaning.
bool SameMeaning(const Item& a, const Item& b, bool arguments)
{
    const auto fields = [](const Item& c)
    {
        return std::tie(c.kind, c.key, c.velocity, c.index, c.value, c.sample);
    };

    if (fields(a) != fields(b))
    {
        return false;
    }

    return !arguments || (a.bytes == b.bytes && a.encoding.opcode == b.encoding.opcode);
}

bool KeepsArguments(Kind kind)
{
    return kind == Kind::kOther || kind == Kind::kLoopStart;
}

// The item's values in the console's units.
Item ToConsole(const Item& item, Revision revision)
{
    Item raw = item;
    raw.velocity = FromUnit(item.velocity, ntdlib::commands::Scale{});
    raw.value.value = FromUnit(item.value.value, ScaleOf(item.kind, revision));
    return raw;
}

// The item's values in the units.
void ToUnits(Item& item, Revision revision)
{
    item.velocity = ToUnit(item.velocity, ntdlib::commands::Scale{});
    item.value.value = ToUnit(item.value.value, ScaleOf(item.kind, revision));
}

// The level of a new pan command, in the revisions whose pan contains a level for each side.
constexpr int64_t kNewPanLevel = 15;

// A value's byte. Throws std::invalid_argument past `high`.
uint8_t ByteOf(int64_t value, int64_t low, int64_t high, Kind kind)
{
    if (value < low || value > high)
    {
        throw std::invalid_argument("a value of a " + std::string(ntdlib::commands::Name(kind)) +
                                    " command doesn't fit in the driver's bytes");
    }

    return uint8_t(value);
}

// The usual bytes of a delay of `delay` frames.
std::vector<uint8_t> UsualDelay(int64_t delay, Revision r)
{
    const auto too_long = []
    {
        return std::invalid_argument("a wait is longer than the driver's delays");
    };

    if (delay < 0)
    {
        throw too_long();
    }

    const bool short_up_to_ef = EternalDuelistLayout(r) || r == Revision::kDungeonDiceMonsters;
    const int64_t one_byte = short_up_to_ef ? 0xEF : 0xDF;
    if (delay <= one_byte)
    {
        return {uint8_t(delay)};
    }

    if (short_up_to_ef)
    {
        if (delay > 0xFFF)
        {
            throw too_long();
        }

        return {uint8_t(0xF0 | (delay >> 8)), uint8_t(delay)};
    }

    if (r == Revision::kUltimateMasters)
    {
        if (delay > 0xFFFF)
        {
            throw too_long();
        }

        return delay <= 0xFFF ? std::vector<uint8_t>{uint8_t(0xE0 | (delay >> 8)), uint8_t(delay)}
                              : std::vector<uint8_t>{0xF0, uint8_t(delay), uint8_t(delay >> 8)};
    }

    if (delay > 0x1FFF)
    {
        throw too_long();
    }

    return {uint8_t(0xE0 | (delay >> 8)), uint8_t(delay)};
}

// The usual opcode of a duty byte or a wave. The bytes that the revision can't contain read back as something else.
uint8_t UsualDutyOrWave(const Item& item, Revision r)
{
    const int64_t value = item.value.value;
    const bool wave = item.index == 1;
    if (r == Revision::kUltimateMasters)
    {
        return (value & 0x3F) == 0 ? uint8_t(0x80 | (value >> 6)) : uint8_t(value + 4);
    }

    if (r == Revision::kDungeonDiceMonsters)
    {
        if (wave)
        {
            return uint8_t(0x70 | value);
        }

        return value <= 0x6F ? uint8_t(value) : uint8_t(0x7C | (value >> 6));
    }

    if (wave)
    {
        return uint8_t(0x80 | (value + 4));
    }
    if (LaterUltimateMasters(r))
    {
        return (value & 0x3F) == 0 ? uint8_t(0x80 | (value >> 6)) : uint8_t(0x80 | (value + 4));
    }

    // The Kitaro revision and those with Rave Master's commands store a duty as it is, and the others in bits 6-7.
    return uint8_t(0x80 | (RaveMasterCommands(r) || r == Revision::kKitaro ? value : value >> 6));
}

// The usual bytes of note `item`, whose values are the console's. Throws std::invalid_argument for a value that no byte
// contains.
std::vector<uint8_t> UsualNoteBytes(const Item& item, Revision r)
{
    const Kind kind = item.kind;
    const bool ultimate = UltimateMasters(r);
    const bool dungeon = r == Revision::kDungeonDiceMonsters;
    const auto byte = [kind](int64_t v)
    {
        return ByteOf(v, 0, 0xFF, kind);
    };

    const int64_t volume = item.velocity;
    if (item.sample)
    {
        const uint16_t sample = *item.sample;
        if (dungeon)
        {
            return {uint8_t((item.index == 1 ? 0xA0 : 0xB0) | ByteOf(volume, 0, 15, kind)), byte(sample)};
        }

        std::vector<uint8_t> bytes;
        const uint8_t first = item.key != 0 ? 0xB0 : 0xA0;
        if (ultimate && volume > 7)
        {
            bytes = {uint8_t(first | 8), byte(volume)};
        }
        else
        {
            bytes = {uint8_t(first | ByteOf(volume, 0, ultimate ? 7 : 15, kind))};
        }

        if (sample > 0xEF && r == Revision::kLiveStart)
        {
            bytes.push_back(uint8_t(0xF0 | ByteOf(sample >> 8, 0, 15, kind)));
            bytes.push_back(uint8_t(sample));
        }
        else
        {
            bytes.push_back(byte(sample));
        }

        if (item.key != 0)
        {
            bytes.push_back(item.key);
        }

        return bytes;
    }

    if (dungeon)
    {
        return {uint8_t((item.index == 1 ? 0x90 : 0xE0) | ByteOf(volume, 0, 15, kind)), item.key};
    }
    if (ultimate && volume > 7)
    {
        return {0xD8, byte(volume), item.key};
    }

    return {uint8_t(0xD0 | ByteOf(volume, 0, ultimate ? 7 : 15, kind)), item.key};
}

// The usual bytes of `item`, whose values are the console's. Throws std::invalid_argument for a value that no byte
// contains. The caller reads the bytes back: a revision without the command reads them as something else.
std::vector<uint8_t> UsualBytes(const Item& item, Revision r)
{
    const Kind kind = item.kind;
    const int64_t value = item.value.value;
    const bool ultimate = UltimateMasters(r);
    const bool dungeon = r == Revision::kDungeonDiceMonsters;
    const auto byte = [kind](int64_t v)
    {
        return ByteOf(v, 0, 0xFF, kind);
    };

    switch (kind)
    {
    case Kind::kWait:
        return UsualDelay(value, r);

    case Kind::kProgram:
        if (item.index == 2)
        {
            return {0xF6, byte(value)};
        }

        return {UsualDutyOrWave(item, r)};

    case Kind::kNote:
        return UsualNoteBytes(item, r);

    case Kind::kVolume:
        if (item.index == 2)
        {
            return {0xF0, uint8_t((ByteOf(item.velocity, 0, 15, kind) << 4) | ByteOf(value, 0, 15, kind))};
        }
        if (dungeon)
        {
            return {uint8_t((item.index == 1 ? 0xD0 : 0xC0) | ByteOf(value, 0, 15, kind))};
        }
        if (ultimate && value > 7)
        {
            return {0xC8, byte(value)};
        }

        return {uint8_t(0xC0 | ByteOf(value, 0, ultimate ? 7 : 15, kind))};

    case Kind::kNoteOff:
        return {uint8_t(dungeon ? 0xFC : 0xE0)};

    case Kind::kRelease:
        return {uint8_t(item.index == 0 ? 0xFB : item.index == 1 ? 0xF8 : 0xF9)};

    case Kind::kPan:
        if (ultimate)
        {
            return {0xF0, byte(value)};
        }
        else
        {
            // The larger level stays on its side, and the other is `value` below it. A new command takes level 15.
            const int64_t level = item.velocity != 0 ? item.velocity : kNewPanLevel;
            const int64_t left = value > 0 ? level - value : level;
            const int64_t right = value < 0 ? level + value : level;
            return {0xF0, uint8_t((ByteOf(left, 0, 15, kind) << 4) | ByteOf(right, 0, 15, kind))};
        }

    case Kind::kLfoDepth:
        if (dungeon)
        {
            return {uint8_t(0x80 | ByteOf(value, 0, 15, kind))};
        }

        return {0xF1, byte(value)};

    case Kind::kPitchBend:
        if (dungeon)
        {
            return {uint8_t(item.index == 0 ? 0xF3 : item.index == 1 ? 0xF2 : 0xF1), byte(value + 32)};
        }

        if (ultimate && value >= -0x400 && (value < -0x40 || value > 0x3F))
        {
            const int64_t held = ByteOf((value + 0x400) >> 8, 0, 15, kind);
            return {0xF2, uint8_t(0x80 | held), uint8_t(value + 0x400)};
        }

        return {0xF2, byte(value + 0x40)};

    case Kind::kLoopStart:
        {
            std::vector<uint8_t> bytes = {uint8_t(dungeon ? 0xFA : 0xF3)};
            bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());
            return bytes;
        }

    case Kind::kLoopEnd:
        return ultimate ? std::vector<uint8_t>{0xFF, 1} : std::vector<uint8_t>{0xFE};

    case Kind::kEnd:
        if (item.index == 2)
        {
            return ultimate ? std::vector<uint8_t>{0xFF, 0} : std::vector<uint8_t>{0xFF};
        }

        return {uint8_t(ultimate && item.index == 0 ? 0xFE : 0xFD)};

    case Kind::kAttack:
        return {0xF4, byte(value)};

    case Kind::kDecay:
        return {0xF5, byte(value)};

    case Kind::kEchoVolume:
        return {0xF7, byte(value)};

    case Kind::kEchoLength:
        return {0xF8, byte(value)};

    case Kind::kMute:
        return {0xFA, byte(value)};

    case Kind::kMainVolume:
        return {0xFB, byte(value)};

    case Kind::kCall:
        // The caller writes the target.
        return {item.key, 0, 0, byte(value)};

    case Kind::kOther:
        {
            std::vector<uint8_t> bytes = {uint8_t(item.encoding.opcode)};
            bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());
            return bytes;
        }

    default:
        throw std::invalid_argument("the driver has no command of kind " + std::string(ntdlib::commands::Name(kind)));
    }
}

// The track that reads an item's bytes the way the item says.
int ReadingTrack(const Item& item)
{
    const bool sample_track =
        (item.kind == Kind::kVolume && item.index == 2) || (item.kind == Kind::kLoopStart && !item.bytes.empty());
    return sample_track ? kSampleTrack : 0;
}

// Returns the item in the console's values that `bytes` read as for `item`'s kind of track, or nothing where they read
// otherwise: more or fewer bytes than they contain.
std::optional<Item> ReadBack(const std::vector<uint8_t>& bytes, bool wait, int track, Revision r)
{
    std::vector<uint8_t> image = bytes;
    image.resize(std::max(image.size(), kLongestItem), 0);
    Rom rom;
    rom.Assign(std::move(image));

    konami::Command c;
    if (wait)
    {
        c.op = Op::kDelay;
        c.length = ReadDelay(rom, kRomBase, r, c.delay);
    }
    else
    {
        c = DecodeCommand(rom, kRomBase, track, r);
    }

    if (c.length != bytes.size())
    {
        return std::nullopt;
    }

    return RawItem(rom, c);
}

// The bytes of `item`, a command or a wait. The item's kept bytes come first, while they read as its values. Throws
// std::invalid_argument where the usual bytes don't read as its values either: the revision doesn't have the command.
std::vector<uint8_t> ItemBytes(const Item& item, Revision r)
{
    // The Ultimate Masters revisions have no wave command. A wave goes in the track's duty byte, and reads back as the
    // command that sets the duty byte. The later builds take a wave from an opcode's low nibble less 4: 0 to 11.
    Item raw = ToConsole(item, r);
    if (UltimateMasters(r) && raw.kind == Kind::kProgram && raw.index == 1)
    {
        if (LaterUltimateMasters(r) && raw.value.value > 11)
        {
            throw std::invalid_argument("revision " + std::string(RevisionName(r)) +
                                        " has no bytes for a wave above 11");
        }

        raw.index = 0;
    }

    const bool wait = item.kind == Kind::kWait;
    const bool arguments = KeepsArguments(item.kind);
    const int track = ReadingTrack(item);
    if (!arguments && (item.encoding.opcode & kKeptBytes))
    {
        std::vector<uint8_t> kept = {uint8_t(item.encoding.opcode)};
        kept.insert(kept.end(), item.bytes.begin(), item.bytes.end());
        const std::optional<Item> read = ReadBack(kept, wait, track, r);
        if (read && SameMeaning(*read, raw, false))
        {
            return kept;
        }
    }

    std::vector<uint8_t> usual = UsualBytes(raw, r);
    Item expected = raw;
    if (!arguments)
    {
        expected.bytes.clear();
    }

    if (item.kind == Kind::kCall)
    {
        expected.index = 0;
    }

    if (item.kind == Kind::kPan && raw.velocity == 0 && !UltimateMasters(r))
    {
        expected.velocity = kNewPanLevel;
    }

    const std::optional<Item> read = ReadBack(usual, wait, track, r);
    if (!read || !SameMeaning(*read, expected, arguments))
    {
        throw std::invalid_argument("revision " + std::string(RevisionName(r)) + " has no bytes for a " +
                                    std::string(ntdlib::commands::Name(item.kind)) + " command with these values");
    }

    return usual;
}

} // namespace
} // namespace agblib::konami
