// SPDX-License-Identifier: MIT

// Song conversion for Nintendo R&D1's MIDI-file sound driver.

#pragma once

#include <string>

#include "agblib/agb/rom.h"
#include "agblib/rd1/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/rd1/soundfont.h"

namespace agblib::sequence_export::rd1
{

using agblib::agb::Rom;
using agblib::rd1::DriverInfo;
using agblib::sequence_export::agb::ConvertSettings;
using agblib::sequence_export::agb::SongReport;

// Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts a song to a MIDI file and a SoundFont. With `shared`, its instruments go there instead of in a SoundFont of
// the song.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

} // namespace agblib::sequence_export::rd1
