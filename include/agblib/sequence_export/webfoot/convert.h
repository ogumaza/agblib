// SPDX-License-Identifier: MIT

// A song's conversion: the model's playback -> MIDI file + SoundFont instruments.
//
// The conversion plays the model and reads each voice after each tick. A note starts where a voice starts its sample,
// and ends where the voice stops or starts it again. Its key is the one nearest the voice's rate, and a bend covers the
// rest. Each tick's levels of the two sides set the channel's pan and expression. A row lasts 120 MIDI ticks, and the
// tempo map gives each row its length. The conversion plays at the music's volume of the song's table.
//
// What MIDI and SoundFont files can't express: the mixer takes each point without interpolation, cuts the sum of the
// voices to 8 bits and clips it, and in the 2002 build wraps the sum of each side at 16 bits. A ping-pong loop of the
// 2002 build turns at its start to a place below the start, up to several hundred points below, and plays the points
// from there. The conversion plays such a loop as a reflection, at the pitch of the voice's step.

#pragma once

#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/webfoot/soundfont.h"
#include "agblib/webfoot/driver.h"

namespace agblib::sequence_export::webfoot
{

using agblib::agb::Rom;
using agblib::sequence_export::agb::ConvertSettings;
using agblib::sequence_export::agb::SongReport;
using agblib::webfoot::DriverInfo;

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
// ConvertSettings::frame_timing has no effect on the conversion. The player's rows and ticks fall on samples rather
// than frames. A song's number counts through the song tables of `info`.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, the song's instruments are added to it for a shared SoundFont; otherwise
// a SoundFont is written next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

// A song's files in memory, and the report of their conversion.
using SongFiles = agblib::sequence_export::agb::ModuleSongFiles;

// Converts one song as ConvertSong() does with a SoundFont of its own, and returns the files' bytes in place of writing
// them. The report names no paths, and `settings.out_dir` and `settings.base_name` don't matter. The files are whole
// only when the report is ok and not silent.
SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

} // namespace agblib::sequence_export::webfoot
