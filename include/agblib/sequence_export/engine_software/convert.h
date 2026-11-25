// SPDX-License-Identifier: MIT

// A song's conversion: the model's playback -> MIDI file + SoundFont instruments.
//
// The conversion plays the model and reads its music channels after each tick. A tick falls on a sample of the mixer's
// output. A note starts where a channel starts its sample, at a note, a retrigger or a sample offset, and ends where
// the channel stops or starts it again. Its key is the one nearest the channel's step, and a bend covers the rest. The
// channel's level sets the note's expression on both sides: the level contains the volume, the music volume, the
// volume envelope and the fade-out. The game mixes mono. Every note therefore plays at the centre, and the songs'
// pans are in no output. A row lasts 120 MIDI ticks, and the tempo map gives each row its length. The conversion
// starts the song as the game's sound manager does: the music volume ramps up from 0, and a jingle plays once at the
// manager's volume and ends.
//
// What MIDI and SoundFont files can't express: the mixer takes each point without interpolation, keeps 8 bits of the
// sum of the channels, and clips it. A volume envelope takes a step each tick, and the expression follows it in steps.
// The zones contain no envelope and no fade-out: the driver's envelopes count ticks, and the ticks follow the tempo.
// Its fade-out falls in a straight line of amplitude. Each note's expression plays both. A SoundFont played alone
// therefore plays each instrument at a steady level.

#pragma once

#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/engine_software/soundfont.h"

namespace agblib::sequence_export::engine_software
{

using agblib::agb::Rom;
using agblib::engine_software::DriverInfo;
using agblib::sequence_export::agb::ConvertSettings;
using agblib::sequence_export::agb::SongReport;

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
// ConvertSettings::frame_timing has no effect on the conversion: the player's rows and ticks fall on samples rather
// than frames. A song's number is its place in `info.songs`.
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

} // namespace agblib::sequence_export::engine_software
