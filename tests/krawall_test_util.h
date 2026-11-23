// SPDX-License-Identifier: MIT

// What Krawall's tests share: the names they use, and the detection of a test cartridge.

#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>

#include "ntdlib/commands/sequence.h"
#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/krawall/archive_player.h"
#include "agblib/krawall/commands.h"
#include "agblib/krawall/driver.h"
#include "agblib/krawall/instruments.h"
#include "agblib/krawall/player.h"
#include "agblib/krawall/song.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/krawall/convert.h"
#include "agblib/sequence_export/krawall/performance.h"
#include "agblib/sequence_export/krawall/soundfont.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/krawall_cart.h"

namespace agblib::sequence_export::krawall
{

using agblib::agb::kRomBase;
using agblib::agb::Rom;
using agblib::krawall::Build;
using agblib::krawall::DetectDriver;
using agblib::krawall::DriverInfo;
using agblib::krawall::DriverOverrides;
using agblib::krawall::DumpSong;
using agblib::krawall::kModeJingle;
using agblib::krawall::kModeLoop;
using agblib::krawall::kModeSong;
using agblib::krawall::kSampleEnd;
using agblib::krawall::kSampleLoop;
using agblib::krawall::kSampleLoopLength;
using agblib::krawall::kSamplePoints;
using agblib::krawall::MixChannel;
using agblib::krawall::ModuleSong;
using agblib::krawall::Player;
using agblib::sequence_export::agb::ConvertSettings;
using agblib::sequence_export::agb::LevelsToControllers;
using agblib::sequence_export::agb::Sf2File;
using agblib::sequence_export::agb::SongReport;
using agblib::testing::agb_files::HasGlobalZoneModulators;
using agblib::testing::agb_files::Le32;
using agblib::testing::agb_files::ReadAll;
using agblib::testing::agb_files::ReadMidi;
using agblib::testing::agb_files::ReadSf2;
using agblib::testing::krawall_cart::Cart;
using agblib::testing::krawall_cart::Cell;
using agblib::testing::krawall_cart::kBidiLoop;
using agblib::testing::krawall_cart::kBidiPoints;
using agblib::testing::krawall_cart::kBreak;
using agblib::testing::krawall_cart::kC4Rate;
using agblib::testing::krawall_cart::kForwardLoop;
using agblib::testing::krawall_cart::kForwardPoints;
using agblib::testing::krawall_cart::kGameInit;
using agblib::testing::krawall_cart::kInit;
using agblib::testing::krawall_cart::kInstrumentTable;
using agblib::testing::krawall_cart::kJump;
using agblib::testing::krawall_cart::kLinearTable;
using agblib::testing::krawall_cart::kMaster;
using agblib::testing::krawall_cart::kModuleTable;
using agblib::testing::krawall_cart::kOneShotPoints;
using agblib::testing::krawall_cart::kPan;
using agblib::testing::krawall_cart::kPeriodTable;
using agblib::testing::krawall_cart::kPlay;
using agblib::testing::krawall_cart::kRampTable;
using agblib::testing::krawall_cart::kSampleTable;
using agblib::testing::krawall_cart::kSetSpeed;
using agblib::testing::krawall_cart::kSineTable;
using agblib::testing::krawall_cart::kSpeed;
using agblib::testing::krawall_cart::kSquareTable;
using agblib::testing::krawall_cart::kTickSamples;
using agblib::testing::krawall_cart::kVersion;
using agblib::testing::krawall_cart::LoopingCells;
namespace cc = seqlib::midi::cc;
namespace sf2 = seqlib::sf2;

inline DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    DetectDriver(rom, DriverOverrides(), info, error);

    return info;
}

} // namespace agblib::sequence_export::krawall
