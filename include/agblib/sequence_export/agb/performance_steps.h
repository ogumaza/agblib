// SPDX-License-Identifier: MIT

// The steps that the GBA drivers' performances for an editor share (each driver's PerformSong()): the stretch that runs
// a command, the times of a performance in frames, the changes of a track's mix, and a track's preset and note events.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/sequence_export/performance.h"

#include "agblib/agb/gba.h"

namespace agblib::sequence_export::agb
{

// The stretch of `stretches` that runs the command at `offset` by `tick`: the last that began by then and contains it,
// or 0 when none does.
inline uint32_t StretchOf(const std::vector<ntdlib::sequence_export::Stretch>& stretches, uint32_t offset,
                          uint32_t tick)
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

// The times of a song that a frame-timed conversion played.
struct FrameTimes
{
    bool truncated = false; // the conversion stopped at its time limit
    bool loops = false;
    uint32_t loop_start = 0; // frames
    uint32_t loop_end = 0;
    uint32_t end_frame = 0;
    uint32_t end_tick = 0;     // the MIDI file's end
    double quarter_frames = 0; // the frames of a quarter note at the song's first tempo, or 0 where it has none
};

// Sets the times of a performance `perf` whose ticks are the driver's frames, from what a conversion played,
// `converted`: whether it stopped at the time limit, its end, its length in seconds, the end of its MIDI file, its loop
// of `loops` passes, and one tempo, with a quarter note of the song's first tempo rounded to whole frames, or 24 frames
// where the song has none.
template <typename Perf>
void SetFrameTimes(Perf& perf, const FrameTimes& converted, int loops)
{
    perf.truncated = converted.truncated;
    perf.end_tick = converted.end_frame;
    perf.seconds = double(converted.end_frame) / agblib::agb::kFrameRate;
    perf.midi_end_tick = converted.end_tick;
    if (converted.loops)
    {
        perf.loop = seqlib::midi::Performance::Loop{converted.loop_start, converted.loop_end};
        perf.loops = loops;
    }

    perf.timebase = uint32_t(std::max(std::lround(converted.quarter_frames), 1L));
    if (converted.quarter_frames <= 0)
    {
        perf.timebase = 24;
    }

    perf.tempo.push_back({0, 60 * agblib::agb::kFrameRate / perf.timebase});
}

// Sets `setting` of a track's mix to `value` at `tick`. A later change of the setting in the same tick replaces an
// earlier one.
inline void AddMixChange(std::vector<ntdlib::sequence_export::MixChange>& changes, uint32_t tick,
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

// Sets the volume of a track's mix to `value` at `tick`, where the track's mix changes only its volume. A later change
// in the same tick replaces an earlier one.
inline void AddVolumeChange(std::vector<ntdlib::sequence_export::MixChange>& changes, uint32_t tick, int value)
{
    if (!changes.empty() && changes.back().tick == tick)
    {
        changes.back().value = int16_t(value);
        return;
    }

    changes.push_back({tick, ntdlib::sequence_export::Mix::kVolume, int16_t(value)});
}

// Adds a change to preset `preset` at `tick` to a track's `events`, unless the track's preset, `current`, is that one
// already. Sets `current`.
inline void AddPresetEvent(std::vector<ntdlib::sequence_export::TrackEvent>& events, std::optional<uint32_t>& current,
                           uint32_t tick, uint32_t preset)
{
    if (current == preset)
    {
        return;
    }

    current = preset;
    ntdlib::sequence_export::TrackEvent event;
    event.tick = tick;
    event.kind = ntdlib::sequence_export::EventKind::kPreset;
    event.preset = preset;
    events.push_back(event);
}

// Adds a note's start, `on`, to a track's `events`, and its end at `off_tick`: a copy of `on` with a velocity of 0.
inline void AddNoteEvents(std::vector<ntdlib::sequence_export::TrackEvent>& events,
                          const ntdlib::sequence_export::TrackEvent& on, uint32_t off_tick)
{
    events.push_back(on);

    ntdlib::sequence_export::TrackEvent off = on;
    off.tick = off_tick;
    off.kind = ntdlib::sequence_export::EventKind::kNoteOff;
    off.value = 0;
    events.push_back(off);
}

// Puts each track's events of `tracks` in the order of their ticks. Events in one tick keep their order.
template <typename Tracks>
void SortTrackEvents(Tracks& tracks)
{
    for (std::vector<ntdlib::sequence_export::TrackEvent>& events : tracks)
    {
        std::stable_sort(events.begin(), events.end(),
                         [](const ntdlib::sequence_export::TrackEvent& a, const ntdlib::sequence_export::TrackEvent& b)
                         { return a.tick < b.tick; });
    }
}

} // namespace agblib::sequence_export::agb
