// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/krawall/performance.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/krawall/commands.h"
#include "agblib/krawall/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "converted_notes.h"

namespace agblib::sequence_export::krawall
{

using agblib::agb::Rom;
using agblib::krawall::DriverInfo;

namespace
{

// The tracks that a performance holds.
constexpr size_t kTracks = 16;

// Sets the value of `kind` in the track's mix to `value` at `tick`, unless it holds that value already. A later change
// in the same tick replaces an earlier one.
void AddChange(std::vector<ntdlib::sequence_export::MixChange>& changes, uint32_t tick,
               ntdlib::sequence_export::Mix kind, int value)
{
    for (size_t i = changes.size(); i-- > 0;)
    {
        if (changes[i].setting != kind)
        {
            continue;
        }
        if (changes[i].value == value)
        {
            return;
        }
        if (changes[i].tick == tick)
        {
            changes[i].value = int16_t(value);
            return;
        }
        break;
    }

    changes.push_back({tick, kind, int16_t(value)});
}

// The stretch of `stretches` that plays the entry at `offset` by `tick`: the last that began by then and holds it.
uint32_t StretchOf(const std::vector<ntdlib::sequence_export::Stretch>& stretches, uint32_t offset, uint32_t tick)
{
    for (size_t i = stretches.size(); i-- > 0;)
    {
        const ntdlib::sequence_export::Stretch& s = stretches[i];
        if (s.tick <= tick && s.first <= offset && offset <= s.last)
        {
            return uint32_t(i);
        }
    }

    return 0;
}

// The offset of the item that holds the byte at `offset`: the last of `items` that starts by then. `items` holds the
// items' offsets in order, or nothing.
uint32_t ItemStart(const std::vector<uint32_t>& items, uint32_t offset)
{
    const auto after = std::upper_bound(items.begin(), items.end(), offset);
    return after == items.begin() ? offset : *(after - 1);
}

// The stretches of rows that `rows` played: one for each row, at the row's tick. A row's speed sets its length, and a
// wait counts rows. A stretch of several rows therefore wouldn't give its rows' ticks. The offsets are those of the
// items that hold the row's first byte and the 0 that ends it.
std::vector<ntdlib::sequence_export::Stretch> Stretches(const std::vector<ConvertedRow>& rows, uint32_t base,
                                                        const std::vector<uint32_t>& items)
{
    std::vector<ntdlib::sequence_export::Stretch> stretches;
    const ConvertedRow* last = nullptr;
    for (const ConvertedRow& row : rows)
    {
        const bool next = last && row.order == last->order && row.row == last->row + 1;
        ntdlib::sequence_export::Stretch s;
        s.tick = uint32_t(row.tick);
        s.first = ItemStart(items, row.data - base);
        s.last = ItemStart(items, std::max(row.end, row.data + 1) - 1 - base);
        s.entry = !last || next ? ntdlib::sequence_export::Entry::kOpen
                  : row.order < last->order || (row.order == last->order && row.row <= last->row)
                      ? ntdlib::sequence_export::Entry::kLoop
                      : ntdlib::sequence_export::Entry::kJump;
        stretches.push_back(s);
        last = &row;
    }

    return stretches;
}

} // namespace

Performance PerformSong(const Rom& rom, const DriverInfo& info, int song, const agb::ConvertSettings& settings)
{
    Performance perf;
    const ConvertedModule module = ConvertedNotes(rom, info, song, settings);
    perf.warnings = module.warnings;
    perf.truncated = module.truncated;
    if (!module.valid)
    {
        return perf;
    }

    const int sequence = agblib::krawall::SequenceOfSong(info, song);
    perf.base = agblib::krawall::SequenceAddress(rom, info, sequence);

    // The offsets of the sequence's items, where the decoder reads it.
    std::vector<uint32_t> items;
    try
    {
        const agblib::krawall::Decoded decoded = agblib::krawall::DecodeRomSequence(rom, info, sequence);
        items = agblib::krawall::ItemOffsets(decoded.sequence, agblib::krawall::TraitsOf(info));
    }
    catch (const std::exception&)
    {
        // The patterns don't lie before the module's header. The offsets stay those of the rows' bytes.
    }
    perf.timebase = module.ticks_per_quarter;
    perf.seconds = module.seconds;
    perf.end_tick = uint32_t(module.end);
    if (module.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{uint32_t(module.loop_start), uint32_t(module.loop_end)};
        perf.loops = settings.loops;
    }
    for (const auto& [tick, micros] : module.tempos)
    {
        if (micros != 0)
        {
            perf.tempo.push_back({uint32_t(tick), 60000000.0 / double(micros)});
        }
    }

    // The channels that the module's rows reach.
    int channels = 0;
    for (const ConvertedLevel& level : module.levels)
    {
        channels = std::max(channels, level.channel + 1);
    }
    const std::vector<ntdlib::sequence_export::Stretch> stretches = Stretches(module.rows, perf.base, items);
    for (size_t t = 0; t < kTracks; t++)
    {
        perf.track_names[t] = "Channel " + std::to_string(t + 1);
        if (int(t) < channels)
        {
            perf.stretches[t] = stretches;
        }
    }

    for (const ConvertedLevel& level : module.levels)
    {
        if (size_t(level.channel) < kTracks)
        {
            std::vector<ntdlib::sequence_export::MixChange>& changes = perf.mix[size_t(level.channel)];
            AddChange(changes, uint32_t(level.tick), ntdlib::sequence_export::Mix::kVolume, level.volume);
            AddChange(changes, uint32_t(level.tick), ntdlib::sequence_export::Mix::kPan, level.pan);
        }
    }

    bool dropped = false;
    std::array<std::optional<uint32_t>, kTracks> preset;
    for (const ConvertedNote& n : module.notes)
    {
        if (size_t(n.channel) >= kTracks)
        {
            dropped = true;
            continue;
        }

        std::vector<ntdlib::sequence_export::TrackEvent>& events = perf.tracks[size_t(n.channel)];
        const auto on_tick = uint32_t(n.on);
        const auto program = uint32_t(std::max(n.program, 0));
        if (preset[size_t(n.channel)] != program)
        {
            preset[size_t(n.channel)] = program;
            ntdlib::sequence_export::TrackEvent event;
            event.tick = on_tick;
            event.kind = ntdlib::sequence_export::EventKind::kPreset;
            event.preset = program;
            events.push_back(event);
        }

        ntdlib::sequence_export::NoteCommand command;
        command.offset = n.entry ? n.entry - perf.base : 0;
        command.stretch = StretchOf(perf.stretches[size_t(n.channel)], command.offset, on_tick);
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);

        ntdlib::sequence_export::TrackEvent on;
        on.tick = on_tick;
        on.kind = ntdlib::sequence_export::EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = 127;
        on.note = id;
        events.push_back(on);

        ntdlib::sequence_export::TrackEvent off = on;
        off.tick = std::max(uint32_t(n.off), on_tick + 1);
        off.kind = ntdlib::sequence_export::EventKind::kNoteOff;
        off.value = 0;
        events.push_back(off);
    }
    if (dropped)
    {
        perf.warnings.push_back("the notes of the channels past the 16th are left out of the performance");
    }

    // Each track's events in the order of their ticks.
    for (std::vector<ntdlib::sequence_export::TrackEvent>& events : perf.tracks)
    {
        std::stable_sort(events.begin(), events.end(),
                         [](const ntdlib::sequence_export::TrackEvent& a, const ntdlib::sequence_export::TrackEvent& b)
                         { return a.tick < b.tick; });
    }

    return perf;
}

} // namespace agblib::sequence_export::krawall
