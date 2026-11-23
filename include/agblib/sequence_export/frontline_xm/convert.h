// SPDX-License-Identifier: MIT

// A song's conversion: the model's playback -> MIDI file + SoundFont instruments.
//
// The conversion plays the model and reads its four channels after each tick. A tick falls on a sample of the mixer's
// output. A note starts where a channel starts its sample, at a note, a delayed note or a sample offset, and ends where
// the channel stops or starts it again. Its key is the one nearest the channel's step, and a bend covers the rest. The
// channel's volume times the song's master volume sets the note's expression, on both sides: the mixer writes FIFO A
// alone, and FIFO A plays on both. A row lasts 120 MIDI ticks, and the tempo map gives each row its length. The
// conversion starts the song as the game does: its list from order 0, at the game's master volume.
//
// What MIDI and SoundFont files can't express: the mixer takes each point without interpolation, keeps 8 bits of the
// sum of the channels, and clips it. Effect 8 without a volume byte sets a volume above 64. Such a volume plays louder
// than full scale and clips. A loop whose points have a fraction plays its loop points rounded to whole points, and an
// instrument whose length isn't its loop's end plays its first pass to the length.

#pragma once

#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/frontline_xm/soundfont.h"

namespace agblib::sequence_export::frontline_xm
{

using agblib::agb::Rom;
using agblib::frontline_xm::DriverInfo;
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

} // namespace agblib::sequence_export::frontline_xm
