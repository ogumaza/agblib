// SPDX-License-Identifier: MIT

// What Quintet's tests share: the names they use, the detection of a test cartridge, and the register writes
// of a song.

#pragma once

#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "agblib/agb/files.h"
#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/quintet/archive_player.h"
#include "agblib/quintet/commands.h"
#include "agblib/quintet/driver.h"
#include "agblib/quintet/instruments.h"
#include "agblib/quintet/sequencer.h"
#include "agblib/quintet/song.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/psg.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/quintet/convert.h"
#include "agblib/sequence_export/quintet/soundfont.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/quintet_cart.h"

namespace agblib::sequence_export::quintet
{

using agblib::agb::kRomBase;
using agblib::agb::RegisterWrite;
using agblib::agb::Rom;
using agblib::agb::Utf8;
using agblib::quintet::ArchivePlayer;
using agblib::quintet::Command;
using agblib::quintet::DecodeCommand;
using agblib::quintet::DetectDriver;
using agblib::quintet::DriverInfo;
using agblib::quintet::DriverOverrides;
using agblib::quintet::DumpSong;
using agblib::quintet::EffectIndex;
using agblib::quintet::Event;
using agblib::quintet::FifoFrames;
using agblib::quintet::FileOfSong;
using agblib::quintet::Instrument;
using agblib::quintet::LookUpInstrument;
using agblib::quintet::ReadMusicFiles;
using agblib::quintet::ReadSample;
using agblib::quintet::ReadWave;
using agblib::quintet::Revision;
using agblib::quintet::Sequencer;
using agblib::quintet::SongAddress;
using agblib::quintet::SongCount;
using agblib::sequence_export::agb::ConvertSettings;
using agblib::sequence_export::agb::kMiddleC;
using agblib::sequence_export::agb::SongNumber;
using agblib::sequence_export::agb::SongReport;
using agblib::sequence_export::agb::SongStem;
using agblib::testing::agb_files::HasGlobalZoneModulators;
using agblib::testing::agb_files::LastWrite;
using agblib::testing::agb_files::Le16;
using agblib::testing::agb_files::Le32;
using agblib::testing::agb_files::MidiEvents;
using agblib::testing::agb_files::ReadAll;
using agblib::testing::agb_files::ReadMidi;
using agblib::testing::agb_files::ReadSf2;
using agblib::testing::agb_files::Sf2Records;
using agblib::testing::agb_files::TempPath;
using agblib::testing::agb_files::TempSettings;
using agblib::testing::quintet_cart::Cart;
using agblib::testing::quintet_cart::Channel;
using agblib::testing::quintet_cart::FileAddress;
using agblib::testing::quintet_cart::kEffectsFile;
using agblib::testing::quintet_cart::kFrequencyTable;
using agblib::testing::quintet_cart::kLengthTable;
using agblib::testing::quintet_cart::kLfoTable;
using agblib::testing::quintet_cart::kMacroFile;
using agblib::testing::quintet_cart::kMusicFile;
using agblib::testing::quintet_cart::kPcmPitchTable;
using agblib::testing::quintet_cart::kPlay;
using agblib::testing::quintet_cart::kPlayEffect;
using agblib::testing::quintet_cart::kSampleFile;
using agblib::testing::quintet_cart::kSfxMacroFile;
using agblib::testing::quintet_cart::kSfxSampleFile;
using agblib::testing::quintet_cart::kWaveFile;
using agblib::testing::quintet_cart::Saw;
using agblib::testing::quintet_cart::Song;

namespace cc = seqlib::midi::cc;

// Returns the driver info of a cartridge, failing the test if detection fails.
inline DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));
    SEQLIB_CHECK(error.empty());

    return info;
}

// Runs a song for `frames` frames and returns each frame's register writes, with the play routine's first.
inline std::vector<std::vector<RegisterWrite>> Writes(const Rom& rom, const DriverInfo& info, int song, int frames)
{
    Sequencer seq(rom, info, song);
    std::vector<std::vector<RegisterWrite>> out = {seq.StartWrites()};
    for (int f = 0; f < frames; f++)
    {
        out.push_back(seq.Step());
    }

    return out;
}

} // namespace agblib::sequence_export::quintet
