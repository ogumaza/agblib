// SPDX-License-Identifier: MIT

// Definitions that the parts of the model of AlphaDream's driver share (sequencer.cpp and sequencer_envelopes.cpp). The
// two files build as one translation unit. Each file also compiles on its own. The definitions keep internal linkage.
// Only those files include this header.

#pragma once

#include <cstdint>

#include "agblib/alphadream/driver.h"
#include "agblib/sequence_export/alphadream/sequencer.h"

namespace agblib::sequence_export::alphadream
{

using agblib::alphadream::kRootKey;
using agblib::alphadream::SampleHeader;
using agblib::alphadream::Version;
using agblib::sequence_export::agb::KeyHz;
using agblib::sequence_export::agb::PsgRegisters;
using agblib::sequence_export::agb::SquareHz;
using agblib::sequence_export::agb::WaveShare;

namespace
{

// The player that the game starts its music on.
constexpr uint8_t kMusicPlayer = 1;

// The level of a full-scale sample at volume 127 and the envelope's full level, with a full-scale FIFO sample at 128.
// The mixer writes it as 63 of the FIFO's 128. The SoundFont's PSG samples contain the PSG's level at volume 15 against
// it. A PSG channel at volume 15 therefore gets the same level.
constexpr double kFullSampleLevel = 63;

// The envelope's full level.
constexpr double kFullEnvelope = 255;

// The points that the mixer makes each frame, at 15768 Hz.
constexpr uint32_t kFramePoints = 264;

// The mixer's positions count 1024ths of a point.
constexpr int kPositionShift = 10;

// The literal that the step routine multiplies by: the share of 2^32 by which the mixer's rate falls short of
// 16384 Hz.
constexpr uint64_t kStepScale = 0x0A080000;

// The PSG channels of the wave and the noise.
constexpr int kWaveChannel = 2;
constexpr int kNoiseChannel = 3;

} // namespace

} // namespace agblib::sequence_export::alphadream
