// SPDX-License-Identifier: MIT

// The steps that the GBA drivers' decoders and encoders of commands share. A decoder's flow follows the tracks through
// a sequence's bytes and keeps the command that it reads at each offset. These steps turn what the flow found into
// items, and check that an encoder's bytes read back as its items.

#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "agblib/agb/data_items.h"
#include "agblib/agb/encoded.h"

namespace agblib::agb
{

// Reads a sequence's bytes one at a time. Past the bytes, it reads 0 and sets a flag.
class ByteReader
{
public:
    ByteReader(std::span<const uint8_t> bytes, uint32_t at) : bytes_(bytes), at_(at)
    {
    }

    uint8_t Byte()
    {
        if (at_ >= bytes_.size())
        {
            past_ = true;
            at_++;
            return 0;
        }

        return bytes_[at_++];
    }

    bool Past() const
    {
        return past_;
    }

    uint32_t At() const
    {
        return at_;
    }

private:
    std::span<const uint8_t> bytes_;
    uint32_t at_;
    bool past_ = false;
};

// Whether a command from offset `at` up to `next` overlaps one of the commands that start at the keys of `items` and
// end at the values of `afters`, with the same keys.
template <typename Item>
bool OverlapsItems(const std::map<uint32_t, Item>& items, const std::map<uint32_t, uint32_t>& afters, uint32_t at,
                   uint32_t next)
{
    const auto after = items.lower_bound(at);
    return (after != items.end() && after->first < next) ||
           (after != items.begin() && afters.at(std::prev(after)->first) > at);
}

// Keeps a sequence whose commands can't be read safely as data, split at the tracks' starts, and says why. `Decoded`
// is a decoder's result, with the fields `sequence`, `entries`, `opaque` and `reason`.
template <typename Decoded>
Decoded OpaqueSequence(std::span<const uint8_t> data, std::span<const uint32_t> starts, std::string reason)
{
    Decoded decoded;
    decoded.opaque = true;
    decoded.reason = std::move(reason);

    const std::map<uint32_t, uint32_t> items =
        AppendDataItems(data, {starts.begin(), starts.end()}, decoded.sequence.items);
    for (const uint32_t start : starts)
    {
        decoded.entries.push_back(items.at(start));
    }

    return decoded;
}

// Appends the commands that `flow` found in `data` to `items`, from offset `at` on, with the bytes between them as data
// items. `flow.Items()` gives each command's offset and item, and `flow.After(offset)` the offset after it.
// `finish(item)` turns an item's values from the driver's into units. Returns the index of each command's item, by
// its offset.
template <typename Flow, typename Finish>
std::map<uint32_t, uint32_t> AppendFlowItems(std::span<const uint8_t> data, uint32_t at, const Flow& flow,
                                             const Finish& finish, std::vector<ntdlib::commands::Command>& items)
{
    std::map<uint32_t, uint32_t> item_at;
    for (const auto& [offset, read] : flow.Items())
    {
        if (offset > at)
        {
            items.push_back(DataItem(data.subspan(at, offset - at)));
        }

        ntdlib::commands::Command item = read;
        finish(item);
        item_at[offset] = uint32_t(items.size());
        items.push_back(std::move(item));
        at = flow.After(offset);
    }

    if (data.size() > at)
    {
        items.push_back(DataItem(data.subspan(at)));
    }

    return item_at;
}

// Makes the items of the sequence in `data`, whose tracks start at `starts`, from what `flow` found as
// AppendFlowItems() does. A problem that the flow found keeps the sequence as data (OpaqueSequence()).
template <typename Decoded, typename Flow, typename Finish>
Decoded BuildSequence(std::span<const uint8_t> data, std::span<const uint32_t> starts, const Flow& flow,
                      const Finish& finish)
{
    if (!flow.Problem().empty())
    {
        return OpaqueSequence<Decoded>(data, starts, flow.Problem());
    }

    Decoded decoded;
    const std::map<uint32_t, uint32_t> item_at = AppendFlowItems(data, 0, flow, finish, decoded.sequence.items);
    for (const uint32_t start : starts)
    {
        decoded.entries.push_back(item_at.at(start));
    }

    return decoded;
}

// Returns each item's offset in the bytes that an encoder writes for `sequence`, and the end. `item_bytes(item)` gives
// a command's bytes.
template <typename ItemBytes>
std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, const ItemBytes& item_bytes)
{
    std::vector<uint32_t> offsets;
    offsets.reserve(sequence.items.size() + 1);
    uint32_t at = 0;
    for (const ntdlib::commands::Command& item : sequence.items)
    {
        offsets.push_back(at);
        at += uint32_t(item.kind == ntdlib::commands::Kind::kData ? item.bytes.size() : item_bytes(item).size());
    }

    offsets.push_back(at);

    return offsets;
}

// Appends the bytes of `items` to `encoded`, and their offsets with the end. `item_bytes(item)` gives a command's
// bytes. Returns whether any item is a command.
template <typename ItemBytes>
bool JoinItems(const std::vector<ntdlib::commands::Command>& items, const ItemBytes& item_bytes, Encoded& encoded)
{
    uint32_t at = 0;
    bool any_command = false;
    for (const ntdlib::commands::Command& item : items)
    {
        const std::vector<uint8_t> bytes = item.kind == ntdlib::commands::Kind::kData ? item.bytes : item_bytes(item);
        encoded.offsets.push_back(at);
        encoded.bytes.insert(encoded.bytes.end(), bytes.begin(), bytes.end());
        at += uint32_t(bytes.size());
        any_command = any_command || item.kind != ntdlib::commands::Kind::kData;
    }

    encoded.offsets.push_back(at);

    return any_command;
}

// Checks that each command that `flow` found in the bytes of `encoded` starts one of `items`, and that `same(kind,
// read)` is true for the item's kind and the kind that the flow read. Throws std::invalid_argument with `unreached` for
// a command that starts no item, and with `other_kind` when the kinds differ.
template <typename Flow, typename Same>
void CheckFlowItems(const std::vector<ntdlib::commands::Command>& items, const Encoded& encoded, const Flow& flow,
                    const char* unreached, const char* other_kind, const Same& same)
{
    std::map<uint32_t, size_t> item_at;
    for (size_t i = 0; i < items.size(); i++)
    {
        if (encoded.offsets[i + 1] > encoded.offsets[i])
        {
            item_at[encoded.offsets[i]] = i;
        }
    }

    for (const auto& [offset, read] : flow.Items())
    {
        const auto found = item_at.find(offset);
        if (found == item_at.end() || items[found->second].kind == ntdlib::commands::Kind::kData)
        {
            throw std::invalid_argument(unreached);
        }
        if (!same(items[found->second].kind, read.kind))
        {
            throw std::invalid_argument(other_kind);
        }
    }
}

// Returns `value`, or throws std::invalid_argument if it isn't from `low` to `high`: the driver's bytes for a value of
// a `kind` command don't contain it.
inline int64_t CommandValue(int64_t value, int64_t low, int64_t high, ntdlib::commands::Kind kind)
{
    if (value < low || value > high)
    {
        throw std::invalid_argument("a value of a " + std::string(ntdlib::commands::Name(kind)) +
                                    " command doesn't fit in the driver's bytes");
    }

    return value;
}

} // namespace agblib::agb
