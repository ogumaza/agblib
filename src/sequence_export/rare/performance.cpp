// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/rare/performance.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"

#include "agblib/agb/rom.h"
#include "agblib/rare/commands.h"
#include "agblib/rare/driver.h"
#include "agblib/sequence_export/agb/performance_steps.h"
#include "agblib/sequence_export/agb/settings.h"
#include "converted_notes.h"

namespace agblib::sequence_export::rare
{

using agblib::agb::Rom;
using agblib::rare::DriverInfo;
using agblib::rare::kTempoScale;

namespace
{

// The tracks that a performance contains.
constexpr size_t kTracks = 16;

} // namespace

Performance PerformSong(const Rom& rom, const DriverInfo& info, int song, const agb::ConvertSettings& settings)
{
    Performance perf;
    const ConvertedTune tune = ConvertedNotes(rom, info, song, settings);
    perf.warnings = tune.warnings;
    perf.truncated = tune.truncated;
    if (!tune.valid)
    {
        return perf;
    }

    perf.base = agblib::rare::SequenceAddress(rom, info, agblib::rare::SequenceOfSong(info, song));
    perf.timebase = tune.ticks_per_quarter;
    perf.seconds = tune.seconds;
    perf.end_tick = uint32_t(tune.end);
    if (tune.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{uint32_t(tune.loop_start), uint32_t(tune.loop_end)};
        perf.loops = settings.loops;
    }

    for (const auto& [tick, tempo] : tune.tempos)
    {
        if (tempo != 0)
        {
            perf.tempo.push_back({uint32_t(tick), 60000000.0 / (double(tempo) * kTempoScale)});
        }
    }

    for (size_t t = 0; t < kTracks; t++)
    {
        perf.track_names[t] = seqlib::midi::DefaultTrackName(int(t));
    }

    // The stretches that began before the end, and each track's first.
    for (const agblib::rare::TrackStretch& s : tune.stretches)
    {
        std::vector<ntdlib::sequence_export::Stretch>& stretches = perf.stretches[s.track];
        if (s.tick >= tune.end && !stretches.empty())
        {
            continue;
        }

        ntdlib::sequence_export::Stretch stretch;
        stretch.tick = uint32_t(s.tick);
        stretch.first = s.first - perf.base;
        stretch.last = s.last - perf.base;
        stretch.entry = s.loop ? ntdlib::sequence_export::Entry::kLoop : ntdlib::sequence_export::Entry::kOpen;
        stretches.push_back(stretch);
    }

    for (const ConvertedVolume& v : tune.volumes)
    {
        if (size_t(v.track) < kTracks)
        {
            agb::AddVolumeChange(perf.mix[size_t(v.track)], uint32_t(v.tick), v.value);
        }
    }

    std::array<std::optional<uint32_t>, kTracks> preset;
    for (const ConvertedNote& n : tune.notes)
    {
        std::vector<ntdlib::sequence_export::TrackEvent>& events = perf.tracks[size_t(n.track)];
        const auto on_tick = uint32_t(n.on);
        const auto program = uint32_t(std::max(n.program, 0));
        agb::AddPresetEvent(events, preset[size_t(n.track)], on_tick, program);

        ntdlib::sequence_export::NoteCommand command;
        command.offset = n.command - perf.base;
        command.stretch = agb::StretchOf(perf.stretches[size_t(n.track)], command.offset, on_tick);
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);

        ntdlib::sequence_export::TrackEvent on;
        on.tick = on_tick;
        on.kind = ntdlib::sequence_export::EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = uint8_t(n.velocity);
        on.note = id;
        agb::AddNoteEvents(events, on, std::max(uint32_t(n.off), on_tick + 1));
    }

    // Each track's events in the order of their ticks.
    agb::SortTrackEvents(perf.tracks);

    return perf;
}

} // namespace agblib::sequence_export::rare
