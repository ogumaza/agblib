// SPDX-License-Identifier: MIT

#include "agblib/konami/commands.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/sequence_codec.h"
#include "commands_items.h"

namespace agblib::konami
{
namespace
{

// How one or more tracks read a byte.
struct Read
{
    uint32_t length = 0;
    bool wait = false;
    int track = 0;
};

// The driver's state of a track at a delay.
struct TrackState
{
    uint16_t start = 0;
    uint32_t pos = 0;
    uint16_t ret = 0;
    uint16_t saved_start = 0;
    uint8_t count = 0;
};

// Follows a song's tracks as the driver plays them, and records what each byte reads as.
class Flow
{
public:
    Flow(const Rom& rom, uint32_t base, uint32_t size, std::span<const uint16_t> offsets, Revision revision)
        : rom_(rom), base_(base), size_(size), offsets_(offsets), revision_(revision)
    {
        std::vector<std::set<uint16_t>> starts(offsets.size());
        for (std::size_t t = 0; t < offsets.size() && problem_.empty(); t++)
        {
            starts_ = &starts[t];
            seen_.clear();
            TrackState state;
            state.start = offsets[t];
            starts_->insert(state.start);
            Follow(int(t), state);
            ran_.push_back(seen_);
        }

        // A song loop sends each track back to the start that it contains, with no call running.
        bool more = loops_;
        while (more && problem_.empty())
        {
            more = false;
            for (std::size_t t = 0; t < offsets.size() && problem_.empty(); t++)
            {
                const std::set<uint16_t> held = starts[t];
                starts_ = &starts[t];
                seen_ = std::move(ran_[t]);
                for (const uint16_t start : held)
                {
                    if (restarted_.insert({int(t), start}).second)
                    {
                        TrackState state;
                        state.start = start;
                        Follow(int(t), state);
                        more = true;
                    }
                }

                ran_[t] = std::move(seen_);
            }
        }

        if (problem_.empty())
        {
            CheckOverlaps();
        }
    }

    const std::string& Problem() const
    {
        return problem_;
    }

    const std::map<uint32_t, Read>& Reads() const
    {
        return reads_;
    }

    // The offset past the last byte that a read took.
    uint32_t End() const
    {
        return end_;
    }

    // Whether the read at `offset` is inside a loop point's bytes.
    bool Inner(uint32_t offset) const
    {
        return inner_.contains(offset);
    }

private:
    struct Key
    {
        friend bool operator==(const Key&, const Key&) = default;
        uint64_t low = 0;
        uint64_t high = 0;
    };

    struct KeyHash
    {
        std::size_t operator()(const Key& key) const
        {
            return std::size_t(key.low * 0x9E3779B97F4A7C15ULL) ^ std::size_t(key.high * 0xC2B2AE3D27D4EB4FULL);
        }
    };

    void Fail(const std::string& what)
    {
        if (problem_.empty())
        {
            problem_ = what;
        }
    }

    // Records a read of `length` bytes. Returns false after a problem.
    bool Add(uint32_t offset, uint32_t length, bool wait, int track, const konami::Command* command)
    {
        if (uint64_t(offset) + length > size_)
        {
            Fail("a track runs past the song's bytes");
            return false;
        }

        const auto [it, added] = reads_.try_emplace(offset, Read{length, wait, track});
        end_ = std::max(end_, offset + length);
        if (added)
        {
            return true;
        }

        const Read& read = it->second;
        if (read.wait != wait)
        {
            Fail("a byte reads both as a delay and as a command");
            return false;
        }

        if (read.length != length)
        {
            Fail("two tracks read a command with other lengths");
            return false;
        }

        if (!command)
        {
            return true;
        }

        if (IsPsgTrack(read.track) != IsPsgTrack(track))
        {
            const konami::Command first = DecodeCommand(rom_, base_ + offset, read.track, revision_);
            if (!SameMeaning(RawItem(rom_, first), RawItem(rom_, *command), true))
            {
                Fail("a PSG track and a sample track read a command with other values");
                return false;
            }
        }

        if (command->op == Op::kCall && offsets_[size_t(read.track)] != offsets_[size_t(track)])
        {
            Fail("two tracks read a call with other targets");
            return false;
        }

        return true;
    }

    void Follow(int track, TrackState s)
    {
        const bool keep = KeepsReturn(revision_);
        for (;;)
        {
            // The delay, then the command after it.
            const uint32_t at = s.start + s.pos;
            if (at >= size_)
            {
                Fail("a track runs past the song's bytes");
                return;
            }

            uint32_t delay;
            const uint32_t delay_size = ReadDelay(rom_, base_ + at, revision_, delay);
            if (!Add(at, delay_size, true, track, nullptr))
            {
                return;
            }

            s.pos = uint16_t(s.pos + delay_size);
            const bool call_runs = s.ret != 0;
            const Key key = {uint64_t(s.start) | uint64_t(s.pos) << 16 | uint64_t(s.ret) << 32 |
                                 uint64_t(call_runs ? s.saved_start : 0) << 48,
                             uint64_t(call_runs ? s.count : 0)};
            if (!seen_.insert(key).second)
            {
                return;
            }

            const uint32_t command_at = s.start + s.pos;
            if (command_at >= size_)
            {
                Fail("a track runs past the song's bytes");
                return;
            }

            const konami::Command c = DecodeCommand(rom_, base_ + command_at, track, revision_);
            if (c.op == Op::kUnknown)
            {
                Fail("a track reaches an opcode that the driver doesn't read");
                return;
            }

            if (!Add(command_at, c.length, false, track, &c))
            {
                return;
            }

            uint32_t pos = s.pos + c.length;
            bool counted = true;
            switch (c.op)
            {
            case Op::kEndTrack:
            case Op::kEndTrackHard:
                return;

            case Op::kJump:
                loops_ = loops_ || c.value != 0;
                return;

            case Op::kCall:
                if (revision_ == Revision::kDungeonDiceMonsters && CallSetsFirst(c.opcode, revision_))
                {
                    // F5 takes its first command from the delay byte after it, whether or not the track ever goes back
                    // there.
                    uint32_t after;
                    const uint32_t after_size = ReadDelay(rom_, base_ + command_at + c.length, revision_, after);
                    if (!Add(command_at + c.length, after_size, true, track, nullptr))
                    {
                        return;
                    }
                }

                s.saved_start = s.start;
                s.ret = uint16_t(pos);
                s.start = offsets_[size_t(track)];
                pos = uint32_t(c.value);
                if (CallSetsFirst(c.opcode, revision_))
                {
                    s.count = uint8_t(c.count + 1);
                }
                else
                {
                    s.count = uint8_t(c.count);
                    counted = false;
                }

                starts_->insert(s.start);
                break;

            case Op::kLoopPoint:
                if (revision_ == Revision::kDungeonDiceMonsters)
                {
                    s.start = uint16_t(s.start + pos - 2);
                    pos = 2;
                }
                else
                {
                    s.start = uint16_t(s.start + pos);
                    pos = 0;
                }

                starts_->insert(s.start);
                break;

            default:
                break;
            }

            if (counted && s.ret != 0)
            {
                s.count--;
                if (s.count == 0)
                {
                    s.start = s.saved_start;
                    pos = s.ret;
                    if (!keep)
                    {
                        s.ret = 0;
                    }

                    starts_->insert(s.start);
                }
            }

            s.pos = pos;
        }
    }

    // Finds reads that overlap. A song loop in Dungeon Dice Monsters reads the two bytes after FA, and those are inside
    // the loop point.
    void CheckOverlaps()
    {
        uint32_t outer_end = 0;
        bool in_loop_point = false;
        for (const auto& [offset, read] : reads_)
        {
            if (offset < outer_end)
            {
                if (!in_loop_point || offset + read.length > outer_end)
                {
                    Fail(in_loop_point ? "a song loop reads a command past its loop point's bytes"
                                       : "commands of the song overlap");
                    return;
                }

                inner_.insert(offset);
                continue;
            }

            outer_end = offset + read.length;
            in_loop_point =
                revision_ == Revision::kDungeonDiceMonsters && !read.wait && rom_.U8(base_ + offset) == 0xFA;
        }
    }

    const Rom& rom_;
    uint32_t base_;
    uint32_t size_;
    std::span<const uint16_t> offsets_;
    Revision revision_;
    std::map<uint32_t, Read> reads_;
    std::set<uint32_t> inner_;
    uint32_t end_ = 0;
    bool loops_ = false;
    std::string problem_;
    std::set<uint16_t>* starts_ = nullptr;
    std::unordered_set<Key, KeyHash> seen_;
    std::vector<std::unordered_set<Key, KeyHash>> ran_;
    std::set<std::pair<int, uint16_t>> restarted_;
};

// The whole song as data, split at the track's starts.
Decoded Opaque(const Rom& rom, uint32_t base, uint32_t size, std::span<const uint16_t> offsets, std::string reason)
{
    Decoded decoded;
    decoded.opaque = true;
    decoded.reason = std::move(reason);

    std::set<uint32_t> splits = {0};
    for (const uint16_t offset : offsets)
    {
        splits.insert(offset);
    }

    std::map<uint32_t, uint32_t> items;
    for (auto it = splits.begin(); it != splits.end(); ++it)
    {
        const auto next = std::next(it);
        const uint32_t end = next == splits.end() ? std::max(size, *it) : *next;
        Item data;
        data.kind = Kind::kData;
        for (uint32_t i = *it; i < end; i++)
        {
            data.bytes.push_back(rom.U8(base + i));
        }

        items[*it] = uint32_t(decoded.sequence.items.size());
        decoded.sequence.items.push_back(std::move(data));
    }

    for (const uint16_t offset : offsets)
    {
        decoded.entries.push_back(items[offset]);
    }

    return decoded;
}

// Decodes the song at `base`, of `size` bytes, from what `flow` found.
Decoded Build(const Rom& rom, uint32_t base, uint32_t size, std::span<const uint16_t> offsets, Revision revision,
              const Flow& flow)
{
    if (!flow.Problem().empty())
    {
        return Opaque(rom, base, size, offsets, flow.Problem());
    }

    Decoded decoded;
    std::vector<Item>& items = decoded.sequence.items;
    std::map<uint32_t, uint32_t> item_at;
    const auto add_data = [&](uint32_t from, uint32_t to)
    {
        if (to > from)
        {
            Item data;
            data.kind = Kind::kData;
            data.bytes.assign(rom.Ptr(base + from), rom.Ptr(base + from) + (to - from));
            items.push_back(std::move(data));
        }
    };

    uint32_t at = 0;
    for (const auto& [offset, read] : flow.Reads())
    {
        if (flow.Inner(offset))
        {
            continue;
        }

        add_data(at, offset);
        at = offset + read.length;

        konami::Command c;
        if (read.wait)
        {
            c.op = Op::kDelay;
            c.addr = base + offset;
            c.length = ReadDelay(rom, c.addr, revision, c.delay);
        }
        else
        {
            c = DecodeCommand(rom, base + offset, read.track, revision);
        }

        Item raw = RawItem(rom, c);
        Item item = raw;
        const std::vector<uint8_t> real(rom.Ptr(base + offset), rom.Ptr(base + offset) + read.length);
        if (!KeepsArguments(item.kind) && item.kind != Kind::kCall)
        {
            std::vector<uint8_t> usual;
            try
            {
                usual = UsualBytes(raw, revision);
            }
            catch (const std::invalid_argument&)
            {
            }

            if (usual != real)
            {
                item.encoding.opcode = uint16_t(kKeptBytes | real[0]);
                item.bytes.assign(real.begin() + 1, real.end());
            }
        }

        if (item.kind == Kind::kCall)
        {
            item.index = uint8_t(read.track);
            item.target = uint32_t(c.value);
            item.target_outside = true; // until the items are known
        }

        ToUnits(item, revision);
        item_at[offset] = uint32_t(items.size());
        items.push_back(std::move(item));
    }

    add_data(at, size);

    // The call's targets as items.
    for (Item& item : items)
    {
        if (item.kind == Kind::kCall)
        {
            const uint32_t target = offsets[item.index] + item.target;
            const auto found = item_at.find(target);
            if (found != item_at.end())
            {
                item.target = found->second;
                item.target_outside = false;
            }
        }
    }

    for (const uint16_t offset : offsets)
    {
        decoded.entries.push_back(item_at.at(offset));
    }

    return decoded;
}

// The offset of each track's start in the song's bytes, from the items in `entries`.
std::vector<uint16_t> TrackStarts(std::span<const uint32_t> offsets, std::span<const uint32_t> entries)
{
    std::vector<uint16_t> starts;
    for (const uint32_t entry : entries)
    {
        if (entry >= offsets.size() - 1)
        {
            throw std::invalid_argument("a track starts past the song's items");
        }
        if (offsets[entry] > 0xFFFF)
        {
            throw std::invalid_argument("a track starts past 0xFFFF bytes from the song's base");
        }

        starts.push_back(uint16_t(offsets[entry]));
    }

    return starts;
}

// The offset that call `item` writes: its target's offset from the start of its track, or the target it gives for one
// outside the song.
uint64_t CallOffset(const Item& item, std::span<const uint32_t> offsets, uint16_t start)
{
    if (item.target_outside)
    {
        if (item.target > 0xFFFF)
        {
            throw std::invalid_argument("a call's offset doesn't fit in the driver's bytes");
        }

        return item.target;
    }

    if (item.target >= offsets.size() - 1)
    {
        throw std::invalid_argument("a call's target lies past the song's items");
    }

    const uint32_t target = offsets[item.target];
    if (target < start || target - start > 0xFFFF)
    {
        throw std::invalid_argument("a call's target lies before its track's start or too far after it");
    }

    return target - start;
}

// Throws std::invalid_argument unless the driver's tracks read `encoded` the way the items of `sequence` say.
void CheckReadBack(const ntdlib::commands::Sequence& sequence, const Encoded& encoded, std::span<const uint16_t> starts,
                   Revision revision)
{
    Rom rom;
    rom.Assign(encoded.bytes);
    const Flow flow(rom, kRomBase, encoded.offsets.back(), starts, revision);
    if (!flow.Problem().empty())
    {
        throw std::invalid_argument("the song's items don't read back: " + flow.Problem());
    }

    std::map<uint32_t, std::size_t> item_at;
    for (std::size_t i = 0; i < sequence.items.size(); i++)
    {
        if (encoded.offsets[i + 1] > encoded.offsets[i])
        {
            item_at[encoded.offsets[i]] = i;
        }
    }

    for (const auto& [offset, read] : flow.Reads())
    {
        if (flow.Inner(offset))
        {
            continue;
        }

        const auto found = item_at.find(offset);
        if (found == item_at.end() || sequence.items[found->second].kind == Kind::kData)
        {
            throw std::invalid_argument("a track reaches bytes that no item starts at");
        }

        if ((sequence.items[found->second].kind == Kind::kWait) != read.wait)
        {
            throw std::invalid_argument(read.wait ? "a track reads a delay where a command lies"
                                                  : "a track reads a command where a wait lies");
        }
    }
}

} // namespace

Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const uint16_t> offsets, Revision revision)
{
    for (const uint16_t offset : offsets)
    {
        if (offset >= data.size())
        {
            throw std::invalid_argument("a track starts outside the song's bytes");
        }
    }

    Rom rom;
    rom.Assign(std::vector<uint8_t>(data.begin(), data.end()));
    const auto size = uint32_t(data.size());
    const Flow flow(rom, kRomBase, size, offsets, revision);

    return Build(rom, kRomBase, size, offsets, revision, flow);
}

Decoded DecodeSong(const Rom& rom, const DriverInfo& info, int song)
{
    SongHeader header;
    if (!ReadSongHeader(rom, info.song_table, song, info.revision, header) || !rom.Contains(header.base))
    {
        throw std::invalid_argument("song " + std::to_string(song) + " has no bytes in the ROM");
    }

    const std::span<const uint16_t> offsets(header.offsets, size_t(header.tracks));
    const Flow flow(rom, header.base, rom.End() - header.base, offsets, info.revision);

    // The song ends with the last byte that a track reads, or at the next song where that is close after it.
    uint32_t size = std::max<uint32_t>(flow.End(), 1);
    uint32_t next = rom.End();
    for (int s = 0; s < info.song_count; s++)
    {
        SongHeader other;
        if (ReadSongHeader(rom, info.song_table, s, info.revision, other) && other.base > header.base + size - 1 &&
            other.base < next)
        {
            next = other.base;
        }
    }

    if (next - (header.base + size) < kSongGap && next < rom.End())
    {
        size = next - header.base;
    }

    return Build(rom, header.base, size, offsets, info.revision, flow);
}

std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, Revision revision)
{
    return agb::ItemOffsets(sequence, [&](const Item& item) { return ItemBytes(item, revision); });
}

Encoded EncodeSequence(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t> entries, Revision revision)
{
    Encoded encoded;
    std::vector<std::vector<uint8_t>> pieces;
    pieces.reserve(sequence.items.size());
    uint32_t at = 0;
    for (const Item& item : sequence.items)
    {
        encoded.offsets.push_back(at);
        pieces.push_back(item.kind == Kind::kData ? item.bytes : ItemBytes(item, revision));
        at += uint32_t(pieces.back().size());
    }

    encoded.offsets.push_back(at);

    const std::vector<uint16_t> starts = TrackStarts(encoded.offsets, entries);

    encoded.bytes.reserve(at);
    bool any_command = false;
    for (std::size_t i = 0; i < pieces.size(); i++)
    {
        const Item& item = sequence.items[i];
        any_command = any_command || item.kind != Kind::kData;
        if (item.kind == Kind::kCall)
        {
            if (item.index >= starts.size())
            {
                throw std::invalid_argument("a call counts from a track that the song lacks");
            }

            const uint64_t value = CallOffset(item, encoded.offsets, starts[item.index]);
            pieces[i][1] = uint8_t(value);
            pieces[i][2] = uint8_t(value >> 8);
        }

        encoded.bytes.insert(encoded.bytes.end(), pieces[i].begin(), pieces[i].end());
    }

    // The driver has to read the bytes as the items say. A song kept as data contains no commands.
    if (any_command)
    {
        CheckReadBack(sequence, encoded, starts, revision);
    }

    return encoded;
}

ntdlib::commands::Scale ScaleOf(Kind kind, Revision revision)
{
    const bool dungeon = revision == Revision::kDungeonDiceMonsters;
    switch (kind)
    {
    case Kind::kPitchBend:
        return {dungeon ? kOne / 32 : kOne / 64, 0};
    case Kind::kPan:
        if (UltimateMasters(revision))
        {
            return {kOne, -64 * kOne};
        }

        return {63 * kOne / 15, 0};
    default:
        return {};
    }
}

Item NewLoopPoint(int track, Revision revision)
{
    Item item;
    item.kind = Kind::kLoopStart;
    const bool extra =
        revision == Revision::kDungeonDiceMonsters || (!IsPsgTrack(track) && !RaveMasterCommands(revision));
    if (extra)
    {
        item.bytes.assign(2, 0);
    }

    return item;
}

} // namespace agblib::konami
