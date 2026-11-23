// SPDX-License-Identifier: MIT

// The steps that the GBA drivers' cuts and joins share for their sequences (each driver's layout.cpp): the size of a
// sequence that a cut reads and whether it can move, the sequences that stay because two of them read the same bytes,
// and the fields that contain addresses.

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/bytes.h"

#include "agblib/agb/layout.h"
#include "agblib/agb/rom.h"

namespace agblib::agb
{

// Whether `size` bytes from `address` meet one of `spans`.
inline bool SpansTouch(std::span<const Span> spans, uint32_t address, uint32_t size)
{
    for (const Span& span : spans)
    {
        if (address < uint64_t(span.start) + span.size && span.start < uint64_t(address) + size)
        {
            return true;
        }
    }

    return false;
}

// A sequence that a cut reads: its size, and whether it can move.
struct CutSize
{
    uint32_t size = 0;   // the bytes that its items encode to, or the bytes of its data where they don't encode
    bool usable = false; // the decoder read it, and its encoding gives its bytes at its address back
};

// The size of the sequence that `decoded` contains, read at `address`, and whether it can move. `encode()` encodes its
// items, and throws std::invalid_argument when they don't encode. The sequence then stays among the ROM's bytes, up to
// the end of its data.
template <typename Decoded, typename Encode>
CutSize SizeOfCut(const Rom& rom, uint32_t address, const Decoded& decoded, const Encode& encode)
{
    CutSize cut;
    for (const ntdlib::commands::Command& item : decoded.sequence.items)
    {
        if (item.kind == ntdlib::commands::Kind::kData)
        {
            cut.size += uint32_t(item.bytes.size());
        }
    }

    try
    {
        const Encoded encoded = encode();
        cut.size = uint32_t(encoded.bytes.size());
        cut.usable = !decoded.opaque && rom.Contains(address, cut.size) &&
                     std::equal(encoded.bytes.begin(), encoded.bytes.end(), rom.Ptr(address));
    }
    catch (const std::invalid_argument&)
    {
    }

    return cut;
}

// The bytes that a driver reads: the spans of its other data, `data`, and the bytes of each of `sequences`, a map from
// a sequence's address to its record, with its size in `size`.
template <typename Sequences>
Reached ReachedOf(const Rom& rom, std::span<const Span> data, const Sequences& sequences)
{
    Reached reached(rom);
    for (const Span& span : data)
    {
        reached.Mark(span.start, span.size);
    }

    for (const auto& [address, sequence] : sequences)
    {
        reached.Mark(address, sequence.size);
    }

    return reached;
}

// Marks the sequences that stay among the ROM's bytes, each once, in the order of their bytes. `sequences` maps a
// sequence's address to its record, with the field `usable`. `size(record)` gives the sequence's bytes. A sequence
// stays when `stays(address, size)` contains, such as for a sequence that contains the driver's other data. When a
// sequence reads any bytes of another, both stay: the other reader would lose them when the sequence moved.
template <typename Sequences, typename Size, typename Stays>
void MarkReadTwice(Sequences& sequences, const Size& size, const Stays& stays)
{
    auto reader = sequences.end(); // the sequence that reads furthest
    uint32_t read_to = 0;          // where that sequence's bytes end
    for (auto it = sequences.begin(); it != sequences.end(); ++it)
    {
        const uint32_t bytes = size(it->second);
        const uint32_t end = it->first + bytes;
        if (stays(it->first, bytes))
        {
            it->second.usable = false;
        }

        if (reader != sequences.end() && it->first < read_to)
        {
            it->second.usable = false;
            reader->second.usable = false;
        }

        if (reader == sequences.end() || end > read_to)
        {
            reader = it;
            read_to = end;
        }
    }
}

// Writes each of `fields` as a 32-bit address in the GBA's map: the distance from the field's second place, the ROM's
// start, to its third, plus kRomBase. Throws std::invalid_argument for an address outside the GBA's address space, and
// the exceptions of JoinedRom::FieldBytes() and JoinedRom::Position().
inline void WriteAddressFields(JoinedRom& rom, std::span<const ntdlib::sar::Field> fields)
{
    for (const ntdlib::sar::Field& field : fields)
    {
        uint8_t* at = rom.FieldBytes(field, 4);
        const uint64_t from = rom.Position(field.from);
        const uint64_t to = rom.Position(field.to);
        if (to < from || to - from + kRomBase > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("a field's address is outside the GBA's address space");
        }

        xsflib::StoreLe32(at, uint32_t(to - from + kRomBase));
    }
}

} // namespace agblib::agb
