// SPDX-License-Identifier: MIT

// What the tests of Rare's driver share: the names they use and the detection of a test cartridge.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "agblib/agb/files.h"
#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/rare/archive_player.h"
#include "agblib/rare/driver.h"
#include "agblib/rare/sequencer.h"
#include "agblib/rare/song.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/rare/convert.h"
#include "agblib/sequence_export/rare/performance.h"
#include "agblib/sequence_export/rare/soundfont.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/rare_cart.h"

namespace agblib::sequence_export::rare
{

using agblib::agb::kFrameRate;
using agblib::agb::kRomBase;
using agblib::agb::PathFromUtf8;
using agblib::agb::Rom;
using agblib::agb::Utf8;
using agblib::rare::Action;
using agblib::rare::ArchivePlayer;
using agblib::rare::DecodeEvent;
using agblib::rare::DescribeEvent;
using agblib::rare::DetectDriver;
using agblib::rare::DriverInfo;
using agblib::rare::DriverOverrides;
using agblib::rare::EnvelopeSettings;
using agblib::rare::Event;
using agblib::rare::FadeFrames;
using agblib::rare::Format;
using agblib::rare::Instrument;
using agblib::rare::kCmdBend;
using agblib::rare::kCmdControl;
using agblib::rare::kCmdDelay1;
using agblib::rare::kCmdDelay2;
using agblib::rare::kCmdDelay3;
using agblib::rare::kCmdEnd;
using agblib::rare::kCmdNoteOff;
using agblib::rare::kCmdNoteOn;
using agblib::rare::kCmdNoteOnB;
using agblib::rare::kCmdProgram;
using agblib::rare::kCmdTempo;
using agblib::rare::kCtrlAttack;
using agblib::rare::kCtrlDecay;
using agblib::rare::kCtrlLoopEnd;
using agblib::rare::kCtrlLoopStart;
using agblib::rare::kCtrlModulation;
using agblib::rare::kCtrlRelease;
using agblib::rare::kCtrlSustain;
using agblib::rare::kCtrlVolume;
using agblib::rare::kFullLevel;
using agblib::rare::kInstDrumKit;
using agblib::rare::kInstKeySplit;
using agblib::rare::kInstSample;
using agblib::rare::kLoopForward;
using agblib::rare::kLoopForwardB;
using agblib::rare::kLoopOnce;
using agblib::rare::kSlotFree;
using agblib::rare::kSlotOn;
using agblib::rare::kSlotReleased;
using agblib::rare::kTempoScale;
using agblib::rare::LookUpInstrument;
using agblib::rare::MixResult;
using agblib::rare::MusicVolume;
using agblib::rare::ReadInstrument;
using agblib::rare::ReadTuneHeader;
using agblib::rare::Sequencer;
using agblib::rare::SilencedChannels;
using agblib::rare::Slot;
using agblib::rare::SongCount;
using agblib::rare::TuneAddress;
using agblib::rare::TuneHeader;
using agblib::sequence_export::agb::kDrumBank;
using agblib::sequence_export::agb::PresetInstrument;
using agblib::sequence_export::agb::VolumeEnvelope;

using agblib::testing::agb_files::HasGlobalZoneModulators;
using agblib::testing::agb_files::Le16;
using agblib::testing::agb_files::Le32;
using agblib::testing::agb_files::MidiEvents;
using agblib::testing::agb_files::ReadAll;
using agblib::testing::agb_files::ReadMidi;
using agblib::testing::agb_files::ReadSf2;
using agblib::testing::agb_files::Sf2Records;
using agblib::testing::agb_files::TempPath;
using agblib::testing::agb_files::TempSettings;
using agblib::testing::rare_cart::Cart;
using agblib::testing::rare_cart::kInit;
using agblib::testing::rare_cart::kPitchTable;
using agblib::testing::rare_cart::kSineTable;
using agblib::testing::rare_cart::kTuneTable;
using agblib::testing::rare_cart::Saw;
using agblib::testing::rare_cart::Track;

// Returns the driver info of a cartridge, failing the test if detection fails.
inline DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    return info;
}

} // namespace agblib::sequence_export::rare
