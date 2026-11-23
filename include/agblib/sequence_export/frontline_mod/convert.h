// SPDX-License-Identifier: MIT

// A song's conversion: the model's playback -> MIDI file + SoundFont instruments.
//
// The conversion plays the model and reads each of the module's channels after each frame's tick or row. A note starts
// where a channel starts its sample, or where a sample offset moves it, and ends where the channel stops or starts it
// again. Its key is the one nearest the channel's step, and a bend covers the rest. The channel's level at each frame
// sets the note's expression, on both sides: the mixer writes FIFO A alone, and FIFO A plays on both. A row lasts 120
// MIDI ticks, and the tempo map gives each row its length. The conversion starts the song as the game does, with the
// game's fade step. A fade-in is then part of the file.
//
// The mixer checks a sample's end once every two samples and restarts a loop with the fraction of the step at 0. By the
// check that restarts a loop, the position can be up to two steps past the loop's end. Each pass therefore lasts N
// samples, the first even count whose points pass the end. A short loop then plays flat of the step's pitch: by up to
// 25 cents in the games' songs. From a note's first restart of its loop, the conversion bends the note to the pitch at
// which the loop's points take N samples, where that differs from the step's pitch by 3 cents or more. The samples stay
// as the game has them.
//
// What MIDI and SoundFont files can't express: the mixer takes each point without interpolation, adds the channels'
// points in 16-bit halves of a word, and maps each sum through the clip table to 8 bits. Within a pass of a loop, it
// plays the points at the step, with points up to about one step past the loop's end: the second point of the last pair
// that it checks. A channel's level changes only at a row. A volume slide on the ticks between rows is heard at the
// next row. A sample without a loop plays a point or two past its end before it stops.

#pragma once

#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/frontline_mod/driver.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/frontline_mod/soundfont.h"

namespace agblib::sequence_export::frontline_mod
{

using agblib::agb::Rom;
using agblib::frontline_mod::DriverInfo;
using agblib::sequence_export::agb::ConvertSettings;
using agblib::sequence_export::agb::SongReport;

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
// ConvertSettings::frame_timing has no effect on the conversion: the rows fall on frames. A song's number is its place
// in `info.songs`.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, the song's instruments are added to it for a shared SoundFont; otherwise
// a SoundFont is written next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

// A song's files in memory, and the report of their conversion.
using SongFiles = agblib::sequence_export::agb::ModuleSongFiles;

// Converts one song as ConvertSong() does with a SoundFont for the song alone, and returns the files' bytes in place of
// writing them. The report names no paths, and `settings.out_dir` and `settings.base_name` don't matter. The files are
// whole only when the report is ok and not silent.
SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

} // namespace agblib::sequence_export::frontline_mod
