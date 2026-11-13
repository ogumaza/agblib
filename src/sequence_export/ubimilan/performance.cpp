// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/ubimilan/performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/ubimilan/soundfont.h"
#include "agblib/ubimilan/driver.h"
#include "agblib/ubimilan/sequencer.h"
#include "converted_notes.h"

namespace agblib::sequence_export::ubimilan
{

using agblib::agb::kFrameRate;
using agblib::agb::Rom;
using agblib::ubimilan::DriverInfo;
using agblib::ubimilan::kKitChannel;
using agblib::ubimilan::kPsgChannels;
using agblib::ubimilan::MusicPiece;
using agblib::ubimilan::ReadMusic;
using agblib::ubimilan::ResourceAddress;
using agblib::ubimilan::SequenceRead;
using seqlib::midi::EventKind;
using seqlib::midi::TrackEvent;

namespace
{

// The track of the noise. No sequence channel 3 plays notes.
constexpr int kNoiseTrack = 3;

std::string TrackNameOf(int channel)
{
    if (channel < kPsgChannels || channel == kKitChannel)
    {
        return ChannelName(channel);
    }

    return channel == kNoiseTrack ? "Noise" : "Channel " + std::to_string(channel);
}

} // namespace

Performance PerformSong(const Rom& rom, const DriverInfo& info, int song, const agb::ConvertSettings& settings)
{
    Performance perf;
    MusicPiece piece;
    if (!ReadMusic(rom, info, song, piece))
    {
        perf.warnings.push_back("its list of sequences can't be read");
        return perf;
    }

    for (const int id : piece.sequences)
    {
        perf.sequences.push_back({id, ResourceAddress(rom, info, id)});
    }

    const ConvertedSong converted = ConvertedNotes(rom, info, song, settings);
    perf.warnings = converted.warnings;
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
    for (size_t channel = 0; channel < perf.tracks.size(); channel++)
    {
        perf.track_names[channel] = TrackNameOf(int(channel));
    }

    // The passes that play. A pass in the piece's last frame belongs to the next pass of the loop.
    bool first = true;
    for (const SequenceRead& read : converted.reads)
    {
        if (read.frame >= converted.end_frame)
        {
            continue;
        }

        ntdlib::sequence_export::Stretch stretch;
        stretch.tick = read.frame;
        stretch.first = agblib::ubimilan::kSequenceHeader;
        stretch.last = read.last - read.base;
        stretch.entry = first             ? ntdlib::sequence_export::Entry::kOpen
                        : read.place == 0 ? ntdlib::sequence_export::Entry::kLoop
                                          : ntdlib::sequence_export::Entry::kJump;
        first = false;
        perf.stretches.push_back(stretch);
        perf.stretch_sequences.push_back(uint32_t(read.place));
    }

    std::array<std::optional<uint32_t>, 16> preset;
    for (const ConvertedNote& n : converted.notes)
    {
        std::vector<TrackEvent>& events = perf.tracks[size_t(n.channel)];
        const uint32_t number = uint32_t(n.bank) * 128 + uint32_t(n.program);
        if (preset[size_t(n.channel)] != number)
        {
            preset[size_t(n.channel)] = number;
            TrackEvent event;
            event.tick = n.on_frame;
            event.kind = EventKind::kPreset;
            event.preset = number;
            events.push_back(event);
        }

        // A piece that is a sample has no sequences. Its notes' commands stay at 0.
        ntdlib::sequence_export::NoteCommand command;
        if (n.read < converted.reads.size())
        {
            command.offset = n.command - converted.reads[n.read].base;
            command.stretch = n.read;
        }
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);
        perf.midi_places.push_back({n.on_tick, n.off_tick});

        TrackEvent on;
        on.tick = n.on_frame;
        on.kind = EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = uint8_t(n.velocity);
        on.note = id;
        events.push_back(on);

        TrackEvent off = on;
        off.tick = n.off_frame;
        off.kind = EventKind::kNoteOff;
        off.value = 0;
        events.push_back(off);
    }

    return perf;
}

} // namespace agblib::sequence_export::ubimilan
