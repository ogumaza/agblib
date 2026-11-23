// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/brownie/performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/brownie/commands.h"
#include "agblib/brownie/driver.h"
#include "agblib/brownie/sequencer.h"
#include "agblib/brownie/song.h"
#include "agblib/sequence_export/agb/performance_steps.h"
#include "agblib/sequence_export/agb/settings.h"
#include "converted_notes.h"

namespace agblib::sequence_export::brownie
{

using agblib::agb::Rom;
using agblib::brownie::ChannelName;
using agblib::brownie::ChannelRead;
using agblib::brownie::CommandOf;
using agblib::brownie::CommandSize;
using agblib::brownie::DriverInfo;
using agblib::brownie::kChannelCount;
using agblib::brownie::kFirstSampleChannel;
using agblib::brownie::LayoutOf;
using agblib::brownie::Region;
using agblib::brownie::Revision;
using agblib::brownie::SongAddress;

namespace
{

// The commands that a stretch's first read can follow.
constexpr uint8_t kRepeatEnd = 0xF5;
constexpr uint8_t kCall = 0xF6;
constexpr uint8_t kReturn = 0xF7;

// The commands that set a channel's mix.
constexpr uint8_t kSamplePan = 0xEA;
constexpr uint8_t kVolume = 0xF2;
constexpr uint8_t kPsgPan = 0xFD;

// Returns the track name of channel `c` of revision `r` as the MIDI file gives it.
std::string TrackNameOf(Revision r, int c)
{
    std::string name = ChannelName(r, c);
    name[0] = char(name[0] - 'a' + 'A');

    return name;
}

// The offsets of the region that contains a sound's list count from this address.
uint32_t BaseOf(const Rom& rom, const DriverInfo& info, int song)
{
    const uint32_t list = SongAddress(rom, info, song);
    for (const Region& region : agblib::brownie::FindRegions(rom, info))
    {
        if (list >= region.start && list < region.start + region.size)
        {
            return region.start;
        }
    }

    return list;
}

// The stretches of each channel's reads. A stretch begins at a channel's first read, and at each read that doesn't
// follow the one before it. The command before it then says how the stretch began. In the sound's last frame, a read
// only finishes the stretch that runs there: a stretch that would begin there belongs to the next pass.
std::array<std::vector<ntdlib::sequence_export::Stretch>, 16> StretchesOf(const Rom& rom, const DriverInfo& info,
                                                                          const std::vector<ChannelRead>& reads,
                                                                          uint32_t base, uint32_t end)
{
    std::array<std::vector<ntdlib::sequence_export::Stretch>, 16> stretches;
    std::array<std::optional<uint32_t>, kChannelCount> last;
    std::array<uint8_t, kChannelCount> depth = {};
    for (const ChannelRead& read : reads)
    {
        if (read.frame > end)
        {
            continue;
        }

        const size_t c = read.channel;
        const std::optional<uint32_t> before = last[c];
        uint8_t command = 0;
        bool follows = false;
        if (before)
        {
            const uint8_t byte = rom.U8(*before);
            command = CommandOf(info.revision, byte);
            follows = read.address == *before + CommandSize(info.revision, byte);
        }

        if (!follows && read.frame == end)
        {
            continue;
        }

        depth[c] = command == kCall ? 1 : command == kReturn ? 0 : depth[c];
        last[c] = read.address;
        const uint32_t offset = read.address - base;
        if (follows)
        {
            stretches[c].back().last = offset;
            continue;
        }

        ntdlib::sequence_export::Stretch stretch;
        stretch.tick = read.frame;
        stretch.first = offset;
        stretch.last = offset;
        stretch.entry = !before                 ? ntdlib::sequence_export::Entry::kOpen
                        : command == kCall      ? ntdlib::sequence_export::Entry::kCall
                        : command == kReturn    ? ntdlib::sequence_export::Entry::kReturn
                        : command == kRepeatEnd ? ntdlib::sequence_export::Entry::kLoop
                                                : ntdlib::sequence_export::Entry::kJump;
        stretch.depth = depth[c];
        stretches[c].push_back(stretch);
    }

    return stretches;
}

// The pan from -64 to 63 of a channel's levels on the left and the right, each from 0 to 15.
int PanOfLevels(int left, int right)
{
    const int difference = right - left;
    return int(std::lround(double(difference) * (difference < 0 ? 64 : 63) / 15));
}

// Each channel's changes of its volume and pan, from the commands that ran before `end`.
std::array<std::vector<ntdlib::sequence_export::MixChange>, 16> MixOf(const Rom& rom, const DriverInfo& info,
                                                                      const std::vector<ChannelRead>& reads,
                                                                      uint32_t end)
{
    std::array<std::vector<ntdlib::sequence_export::MixChange>, 16> mix;
    for (const ChannelRead& read : reads)
    {
        if (read.frame >= end)
        {
            continue;
        }

        std::vector<ntdlib::sequence_export::MixChange>& changes = mix[read.channel];
        const uint8_t byte = rom.U8(read.address);
        if (byte < 0xE0)
        {
            continue;
        }

        const uint8_t value = rom.U8(read.address + 1);
        switch (CommandOf(info.revision, byte))
        {
        case kVolume:
            agb::AddMixChange(changes, read.frame, ntdlib::sequence_export::Mix::kVolume, value);
            break;

        case kSamplePan:
            // SetVoiceLevels and a note read these levels only in the Sword of Mana revision.
            if (read.channel >= kFirstSampleChannel && info.revision != Revision::kMagicalVacation)
            {
                agb::AddMixChange(changes, read.frame, ntdlib::sequence_export::Mix::kPan,
                                  PanOfLevels(value >> 4, value & 15));
            }
            break;

        case kPsgPan:
            // The driver shifts the byte's first bits of NR51 to the channel's.
            if (read.channel < kFirstSampleChannel)
            {
                agb::AddMixChange(changes, read.frame, ntdlib::sequence_export::Mix::kPan,
                                  PanOfLevels(((value >> 4) & 1) * 15, (value & 1) * 15));
            }
            break;

        default:
            break;
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
    if (converted.end_frame == 0 && converted.reads.empty())
    {
        return perf;
    }

    perf.base = BaseOf(rom, info, song);
    agb::SetFrameTimes(perf, converted, settings.loops);
    for (int c = 0; c < LayoutOf(info.revision).channels; c++)
    {
        perf.track_names[size_t(c)] = TrackNameOf(info.revision, c);
    }

    perf.stretches = StretchesOf(rom, info, converted.reads, perf.base, converted.end_frame);
    perf.mix = MixOf(rom, info, converted.reads, converted.end_frame);

    std::array<std::optional<uint32_t>, kChannelCount> preset;
    for (const ConvertedNote& n : converted.notes)
    {
        std::vector<ntdlib::sequence_export::TrackEvent>& events = perf.tracks[size_t(n.track)];
        const uint32_t number = uint32_t(n.bank) * 128 + uint32_t(n.program);
        agb::AddPresetEvent(events, preset[size_t(n.track)], n.on_frame, number);

        ntdlib::sequence_export::NoteCommand command;
        command.offset = n.command - perf.base;
        command.stretch = agb::StretchOf(perf.stretches[size_t(n.track)], command.offset, n.command_frame);
        const uint32_t id = uint32_t(perf.note_commands.size()) + 1;
        perf.note_commands.push_back(command);
        perf.note_causes.push_back(n.on_frame == n.command_frame ? NoteCause::kNote : NoteCause::kEarlier);
        perf.midi_places.push_back({n.on_tick, n.off_tick});

        ntdlib::sequence_export::TrackEvent on;
        on.tick = n.on_frame;
        on.kind = ntdlib::sequence_export::EventKind::kNoteOn;
        on.key = uint8_t(n.key);
        on.value = 127;
        on.note = id;
        agb::AddNoteEvents(events, on, n.off_frame);
    }

    return perf;
}

} // namespace agblib::sequence_export::brownie
