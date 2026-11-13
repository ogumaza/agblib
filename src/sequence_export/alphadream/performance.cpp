// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/alphadream/performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/alphadream/commands.h"
#include "agblib/alphadream/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/alphadream/sequencer.h"
#include "converted_notes.h"

namespace agblib::sequence_export::alphadream
{

using agblib::agb::kFrameRate;
using agblib::alphadream::CommandSize;
using agblib::alphadream::StartsNote;
using agblib::alphadream::Version;

namespace
{

using TrackStretches = std::array<std::vector<ntdlib::sequence_export::Stretch>, kTracks>;
using TrackMix = std::array<std::vector<ntdlib::sequence_export::MixChange>, kTracks>;

// The commands that jump, and those that set a track's mix.
constexpr uint8_t kCountedJump = 0xE8; // E8 to EF
constexpr uint8_t kVolume = 0xF1;
constexpr uint8_t kPan = 0xF2;
constexpr uint8_t kGoto = 0xF8;

// The track's volume before a command sets it.
constexpr int kStartVolume = 0xC8;

// The bytes of a read.
uint32_t SizeOf(const Rom& rom, Version version, const TrackRead& read)
{
    return read.ticks ? 1 : CommandSize(rom.U8(read.address), version);
}

// Whether a read is of a command that jumps.
bool Jumps(const Rom& rom, const TrackRead& read)
{
    const uint8_t b = rom.U8(read.address);
    return !read.ticks && (b == kGoto || (b >= kCountedJump && b < kCountedJump + 8));
}

// The stretches of each track's reads. A stretch begins at a track's first read, and at each read that doesn't follow
// the one before it: the command before it jumped. In the song's last frame, a read only finishes the stretch that
// runs there. A stretch that would begin there belongs to the next pass.
TrackStretches StretchesOf(const Rom& rom, Version version, const std::vector<TrackRead>& reads, uint32_t base,
                           uint32_t end)
{
    TrackStretches stretches;
    std::array<std::optional<TrackRead>, kTracks> last;
    for (const TrackRead& read : reads)
    {
        if (read.frame > end || read.track >= kTracks)
        {
            continue;
        }

        const size_t t = read.track;
        const std::optional<TrackRead> before = last[t];
        const bool follows = before && read.address == before->address + SizeOf(rom, version, *before);
        if (!follows && read.frame == end)
        {
            continue;
        }

        last[t] = read;
        const uint32_t offset = read.address - base;
        if (follows)
        {
            stretches[t].back().last = offset;
            continue;
        }

        ntdlib::sequence_export::Stretch stretch;
        stretch.tick = read.frame;
        stretch.first = offset;
        stretch.last = offset;
        stretch.entry = !before                           ? ntdlib::sequence_export::Entry::kOpen
                        : !Jumps(rom, *before)            ? ntdlib::sequence_export::Entry::kJump
                        : read.address <= before->address ? ntdlib::sequence_export::Entry::kLoop
                                                          : ntdlib::sequence_export::Entry::kJump;
        stretches[t].push_back(stretch);
    }

    return stretches;
}

// Sets `setting` of the track's mix to `value` at `tick`. A later change in the same tick replaces an earlier one.
void AddChange(std::vector<ntdlib::sequence_export::MixChange>& changes, uint32_t tick,
               ntdlib::sequence_export::Mix setting, int value)
{
    const auto same = std::find_if(changes.rbegin(), changes.rend(), [&](const ntdlib::sequence_export::MixChange& c)
                                   { return c.tick == tick && c.setting == setting; });
    if (same != changes.rend())
    {
        same->value = int16_t(value);
    }
    else
    {
        changes.push_back({tick, setting, int16_t(value)});
    }
}

// The pan from -64 to 63 of F2's byte: 0 at the right, 0x7F and 0x80 at the centre, 0xFF at the left.
int PanOf(int byte)
{
    return byte <= 0x7F ? int(std::lround((0x7F - byte) * 63.0 / 0x7F))
                        : -int(std::lround((byte - 0x80) * 64.0 / 0x7F));
}

// Each track's changes of its volume and pan, from the commands and notes that it read before `end`.
TrackMix MixOf(const Rom& rom, Version version, const std::vector<TrackRead>& reads, uint32_t end)
{
    TrackMix mix;
    std::array<int, kTracks> volume;
    volume.fill(kStartVolume);
    for (const TrackRead& read : reads)
    {
        if (read.frame >= end || read.track >= kTracks || read.ticks)
        {
            continue;
        }

        const size_t t = read.track;
        const uint8_t b = rom.U8(read.address);
        if (StartsNote(b, version))
        {
            // B85A's and A84J's notes hold the track's volume after their keys.
            if (version != Version::kSuperstarSaga && rom.U8(read.address + 2) != volume[t])
            {
                volume[t] = rom.U8(read.address + 2);
                AddChange(mix[t], read.frame, ntdlib::sequence_export::Mix::kVolume, volume[t]);
            }
        }
        else if (b == kVolume)
        {
            volume[t] = rom.U8(read.address + 1);
            AddChange(mix[t], read.frame, ntdlib::sequence_export::Mix::kVolume, volume[t]);
        }
        else if (b == kPan)
        {
            AddChange(mix[t], read.frame, ntdlib::sequence_export::Mix::kPan, PanOf(rom.U8(read.address + 1)));
        }
    }

    return mix;
}

// The stretch of `stretches` that runs the read at `offset` by `tick`: the last that began by then and holds it.
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

} // namespace

Performance PerformSong(const Rom& rom, const DriverInfo& info, int song, const agb::ConvertSettings& settings)
{
    Performance perf;
    const ConvertedSong converted = ConvertedNotes(rom, info, song, settings);
    perf.warnings = converted.warnings;
    if (!converted.valid)
    {
        return perf;
    }

    perf.base = converted.address;
    perf.truncated = converted.truncated;
    perf.end_tick = converted.end_frame;
    perf.seconds = double(converted.end_frame) / kFrameRate;
    perf.midi_end_tick = converted.end_tick;
    if (converted.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{converted.loop_start, converted.loop_end};
        perf.loops = settings.loops;
    }

    perf.timebase = uint32_t(std::max(std::lround(converted.quarter_frames), 1L));
    if (converted.quarter_frames <= 0)
    {
        perf.timebase = 24;
    }
    perf.tempo.push_back({0, 60 * kFrameRate / perf.timebase});
    for (int t = 0; t < kTracks; t++)
    {
        perf.track_names[size_t(t)] = converted.track_names[size_t(t)];
    }

    perf.stretches = StretchesOf(rom, info.version, converted.reads, perf.base, converted.end_frame);
    perf.mix = MixOf(rom, info.version, converted.reads, converted.end_frame);

    std::array<std::optional<uint32_t>, kTracks> preset;
    for (const ConvertedNote& n : converted.notes)
    {
        const auto t = size_t(n.track);
        std::vector<ntdlib::sequence_export::TrackEvent>& events = perf.tracks[t];
        const auto number = uint32_t(n.preset);
        if (preset[t] != number)
        {
            preset[t] = number;
            ntdlib::sequence_export::TrackEvent event;
            event.tick = n.on_frame;
            event.kind = ntdlib::sequence_export::EventKind::kPreset;
            event.preset = number;
            events.push_back(event);
        }

        // The channel's restore starts a note without a command.
        ntdlib::sequence_export::NoteCommand command;
        if (n.command)
        {
            command.offset = n.command - perf.base;
            command.stretch = StretchOf(perf.stretches[t], command.offset, n.on_frame);
        }
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);
        perf.midi_places.push_back({n.on_tick, n.off_tick});

        ntdlib::sequence_export::TrackEvent on;
        on.tick = n.on_frame;
        on.kind = ntdlib::sequence_export::EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = 127;
        on.note = id;
        events.push_back(on);

        ntdlib::sequence_export::TrackEvent off = on;
        off.tick = n.off_frame;
        off.kind = ntdlib::sequence_export::EventKind::kNoteOff;
        off.value = 0;
        events.push_back(off);
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

} // namespace agblib::sequence_export::alphadream
