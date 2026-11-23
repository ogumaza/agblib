// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/rd1/performance.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/rd1/commands.h"
#include "agblib/rd1/driver.h"
#include "agblib/rd1/sequencer.h"
#include "agblib/sequence_export/agb/performance_steps.h"
#include "agblib/sequence_export/agb/settings.h"
#include "converted_notes.h"

namespace agblib::sequence_export::rd1
{

using agblib::agb::kFrameRate;
using agblib::agb::Rom;
using agblib::rd1::DriverInfo;
using agblib::rd1::kMaxTracks;
using agblib::rd1::TrackRead;

namespace
{

constexpr int kControlStatus = 0xB0;
constexpr int kVolumeController = 7;
constexpr int kPanController = 10;

// CC10's value at the centre.
constexpr int kCentre = 64;

using TrackStretches = std::array<std::vector<ntdlib::sequence_export::Stretch>, kMaxTracks>;
using TrackMix = std::array<std::vector<ntdlib::sequence_export::MixChange>, kMaxTracks>;

// The stretches of each track's reads. A stretch begins at a track's first read, and at each read that doesn't follow
// the one before it: the loop took the track back. At the song's end, a read only finishes the stretch that runs
// there. A stretch that would begin there belongs to the next pass.
TrackStretches StretchesOf(const std::vector<TrackRead>& reads, uint32_t base, uint64_t end)
{
    TrackStretches stretches;
    std::array<const TrackRead*, kMaxTracks> last = {};
    for (const TrackRead& read : reads)
    {
        if (read.tick > end || read.track < 0 || read.track >= kMaxTracks)
        {
            continue;
        }

        const auto t = size_t(read.track);
        const TrackRead* before = last[t];
        const bool follows = before && read.address == before->next;
        if (!follows && read.tick == end)
        {
            continue;
        }

        last[t] = &read;
        if (follows)
        {
            stretches[t].back().last = read.address - base;
            continue;
        }

        ntdlib::sequence_export::Stretch stretch;
        stretch.tick = uint32_t(read.tick);
        stretch.first = read.address - base;
        stretch.last = stretch.first;
        stretch.entry = before ? ntdlib::sequence_export::Entry::kLoop : ntdlib::sequence_export::Entry::kOpen;
        stretches[t].push_back(stretch);
    }

    return stretches;
}

// Each track's changes of its volume and pan, from the controllers that it read before the song's end. An event that
// leaves out its status byte starts with its first data byte.
TrackMix MixOf(const Rom& rom, const std::vector<TrackRead>& reads, uint64_t end)
{
    TrackMix mix;
    for (const TrackRead& read : reads)
    {
        if (read.tick >= end || read.track < 0 || read.track >= kMaxTracks || (read.status & 0xF0) != kControlStatus)
        {
            continue;
        }

        const uint32_t data = read.address + ((rom.U8(read.address) & 0x80) ? 1 : 0);
        const int controller = rom.U8(data);
        const int value = rom.U8(data + 1) & 0x7F;
        std::vector<ntdlib::sequence_export::MixChange>& changes = mix[size_t(read.track)];
        if (controller == kVolumeController)
        {
            agb::AddMixChange(changes, uint32_t(read.tick), ntdlib::sequence_export::Mix::kVolume, value);
        }
        else if (controller == kPanController)
        {
            agb::AddMixChange(changes, uint32_t(read.tick), ntdlib::sequence_export::Mix::kPan, value - kCentre);
        }
    }

    return mix;
}

} // namespace

Performance PerformSong(const Rom& rom, const DriverInfo& info, int song, const agb::ConvertSettings& settings)
{
    Performance perf;
    const ConvertedSong converted = ConvertedNotes(rom, info, song, settings);
    perf.warnings = converted.warnings;
    perf.truncated = converted.truncated;
    if (!converted.valid)
    {
        return perf;
    }

    perf.base = converted.midi;
    perf.seconds = converted.seconds;
    perf.end_tick = uint32_t(converted.end);
    perf.midi_end_tick = converted.end_tick;
    if (converted.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{uint32_t(converted.loop_start), uint32_t(converted.loop_end)};
        perf.loops = settings.loops;
    }

    perf.timebase = uint32_t(converted.division);
    bool first = true;
    for (const auto& [tick, step] : converted.tempos)
    {
        // A frame moves the song on by `step` 256ths of a tick. The first tempo goes at the start. The MIDI file puts
        // it there too.
        if (tick < converted.end || first)
        {
            perf.tempo.push_back(
                {first ? 0 : uint32_t(tick), 60.0 * kFrameRate * double(step) / (256.0 * double(converted.division))});
        }

        first = false;
    }

    for (int c = 0; c < 16; c++)
    {
        perf.track_names[size_t(c)] = "Channel " + std::to_string(c);
    }

    perf.stretches = StretchesOf(converted.reads, perf.base, converted.end);
    perf.mix = MixOf(rom, converted.reads, converted.end);

    std::array<std::optional<uint32_t>, 16> preset;
    for (const ConvertedNote& n : converted.notes)
    {
        const auto c = size_t(n.channel & 15);
        std::vector<ntdlib::sequence_export::TrackEvent>& events = perf.tracks[c];
        const auto tick = uint32_t(n.on);
        const uint32_t number = uint32_t(converted.bank) * 128 + uint32_t(n.program);
        agb::AddPresetEvent(events, preset[c], tick, number);

        ntdlib::sequence_export::NoteCommand command;
        command.offset = n.command - perf.base;
        if (n.track >= 0 && n.track < kMaxTracks)
        {
            command.stretch = agb::StretchOf(perf.stretches[size_t(n.track)], command.offset, tick);
        }

        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);
        perf.note_tracks.push_back(uint8_t(n.track));
        perf.midi_places.push_back({n.on_tick, n.off_tick});

        ntdlib::sequence_export::TrackEvent on;
        on.tick = tick;
        on.kind = ntdlib::sequence_export::EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = uint8_t(n.velocity);
        on.note = id;
        agb::AddNoteEvents(events, on, std::max(uint32_t(n.off), tick + 1));
    }

    // Each track's events in the order of their ticks.
    agb::SortTrackEvents(perf.tracks);

    return perf;
}

} // namespace agblib::sequence_export::rd1
