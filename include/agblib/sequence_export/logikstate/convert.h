// SPDX-License-Identifier: MIT

// Module conversion: the model's playback -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/logikstate/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/logikstate/soundfont.h"

namespace agblib::sequence_export::logikstate
{

using agblib::agb::Rom;
using agblib::logikstate::DriverInfo;
using agblib::sequence_export::agb::ConvertSettings;
using agblib::sequence_export::agb::SongReport;

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
// ConvertSettings::frame_timing has no effect on a module's conversion. The player's rows and ticks fall on samples
// rather than frames.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, the song's instruments are added to it for a shared SoundFont; otherwise
// a SoundFont is written next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

// A song's files in memory, and the report of their conversion.
struct SongFiles
{
    SongReport report;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
};

// Converts one song as ConvertSong() does with a SoundFont of its own, and returns the files' bytes in place of writing
// them. The report names no paths, and `settings.out_dir` and `settings.base_name` don't matter. The files are whole
// only when the report is ok and not silent.
SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

} // namespace agblib::sequence_export::logikstate
