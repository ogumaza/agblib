// SPDX-License-Identifier: MIT

// The notes that the conversion of a song of AlphaDream's driver writes, for the song's performance.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/alphadream/driver.h"
#include "agblib/sequence_export/agb/performance_steps.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/alphadream/sequencer.h"

namespace agblib::sequence_export::alphadream
{

// A note of the MIDI file, with the frames that the driver plays it in, its ticks in the file and the command behind
// it.
struct ConvertedNote
{
    int track = 0;
    uint32_t on_frame = 0;
    uint32_t off_frame = 0;
    uint32_t on_tick = 0;
    uint32_t off_tick = 0;
    int key = 0;
    int preset = 0;       // the note's preset in the SoundFont: PresetNumber() of the track's kind and the instrument
    uint32_t command = 0; // the address of the note's bytes, or 0 for a note that the channel's restore starts
};

// A song as its conversion plays it.
struct ConvertedSong : agb::FrameTimes
{
    // end_tick is the song's end, or a tick later for a note that starts on its last tick.
    bool valid = false; // the song table names a song that plays a track
    std::vector<std::string> warnings;
    uint32_t address = 0; // the song's header
    std::array<std::string, kTracks> track_names;
    std::vector<TrackRead> reads;     // what the tracks read, in the order that the driver read it
    std::vector<ConvertedNote> notes; // each track's notes in order, the tracks in order
};

// Plays `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result.
ConvertedSong ConvertedNotes(const Rom& rom, const DriverInfo& info, int song, const agb::ConvertSettings& settings);

} // namespace agblib::sequence_export::alphadream
