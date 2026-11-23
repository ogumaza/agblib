// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/quintet/performance.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/quintet/commands.h"
#include "agblib/quintet/driver.h"
#include "agblib/sequence_export/agb/performance_steps.h"
#include "agblib/sequence_export/agb/settings.h"
#include "converted_notes.h"

namespace agblib::sequence_export::quintet
{

using agblib::agb::Rom;
using agblib::quintet::DriverInfo;

namespace
{

// The names of the channels' tracks.
constexpr const char* kTrackNames[] = {"Square 1", "Square 2", "Wave", "Noise", "PCM A", "PCM B"};

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

    perf.base = agblib::quintet::SongAddress(info, song) + agblib::quintet::kHeaderBytes;
    perf.timebase = converted.ticks_per_quarter;
    perf.seconds = converted.seconds;
    perf.end_tick = converted.end;
    if (converted.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{converted.loop_start, converted.loop_end};
        perf.loops = settings.loops;
    }

    for (const auto& [tick, micros] : converted.tempos)
    {
        perf.tempo.push_back({tick, 60000000.0 / double(micros)});
    }

    for (size_t t = 0; t < std::size(kTrackNames); t++)
    {
        perf.track_names[t] = kTrackNames[t];
    }

    // The stretches that began before the end, and each channel's first.
    for (const ConvertedStretch& s : converted.stretches)
    {
        std::vector<ntdlib::sequence_export::Stretch>& stretches = perf.stretches[size_t(s.channel)];
        if (s.tick >= converted.end && !stretches.empty())
        {
            continue;
        }

        ntdlib::sequence_export::Stretch stretch;
        stretch.tick = s.tick;
        stretch.first = s.first - perf.base;
        stretch.last = s.last - perf.base;
        stretch.entry = s.loop ? ntdlib::sequence_export::Entry::kLoop : ntdlib::sequence_export::Entry::kOpen;
        stretch.depth = uint8_t(s.depth);
        stretch.note_wait = true;
        stretches.push_back(stretch);
    }

    for (const ConvertedVolume& v : converted.volumes)
    {
        agb::AddVolumeChange(perf.mix[size_t(v.channel)], v.tick, v.value);
    }

    std::array<std::optional<uint32_t>, std::size(kTrackNames)> preset;
    for (const ConvertedNote& n : converted.notes)
    {
        std::vector<ntdlib::sequence_export::TrackEvent>& events = perf.tracks[size_t(n.channel)];
        const auto program = uint32_t(n.program);
        agb::AddPresetEvent(events, preset[size_t(n.channel)], n.on, program);

        ntdlib::sequence_export::NoteCommand command;
        command.offset = n.command - perf.base;
        command.stretch = agb::StretchOf(perf.stretches[size_t(n.channel)], command.offset, n.on);
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);

        ntdlib::sequence_export::TrackEvent on;
        on.tick = n.on;
        on.kind = ntdlib::sequence_export::EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = 127;
        on.note = id;
        agb::AddNoteEvents(events, on, n.off);
    }

    // Each track's events in the order of their ticks.
    agb::SortTrackEvents(perf.tracks);

    return perf;
}

} // namespace agblib::sequence_export::quintet
