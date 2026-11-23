// SPDX-License-Identifier: MIT

// Definitions that the parts of the R&D2 sequencer share (sequencer.cpp, sequencer_voices.cpp, sequencer_levels.cpp and
// sequencer_output.cpp): the sound registers, the limits, the arithmetic of the driver and the events. The four files
// build as one translation unit. The definitions therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/sound_registers.h"
#include "agblib/agb/warnings.h"
#include "agblib/rd2/driver.h"
#include "agblib/rd2/sequencer.h"

namespace agblib::rd2
{

namespace
{

using agblib::agb::Hex;
using agblib::agb::WarnOnce;

// The NRx2 value with which the PSG voices leave a channel's envelope as it is.
constexpr uint8_t kNoEnvelope = 8;

// Each PSG channel's NRx4. A write that sets its bit 7 starts the channel again.
constexpr uint32_t kRestartRegisters[kPsgVoices] = {agb::kRegNr14, agb::kRegNr24, agb::kRegNr34, agb::kRegNr44};
constexpr uint8_t kRestartBit = 0x80;

// The time of a track that F8 has started and that hasn't run yet.
constexpr uint64_t kFresh = ~uint64_t(0);

// The most commands a track reads in a frame before the model gives up on it. A loop without a wait then can't hang the
// conversion. The driver would hang there.
constexpr int kMaxCommands = 100000;

// The tuning of the region the driver keeps in RAM for instruments with a sample for each key. The region plays a note
// of 0x30 at the sample's rate. It's a sample voice's, with no flags and no release.
constexpr uint8_t kKeySampleTuning = 0x30;

// Returns a / b as the driver's division does. libgcc's division gives 0 for a divisor of 0.
[[maybe_unused]] uint32_t UnsignedDiv(uint32_t a, uint32_t b)
{
    return b ? a / b : 0;
}

// Returns a / b, rounded towards 0 as the driver's signed divisions do. A divisor of 0 gives 0 as in libgcc's
// division. The driver's mixer uses the BIOS's Div instead. Div never returns for a divisor of 0.
[[maybe_unused]] int32_t SignedDiv(int32_t a, int32_t b)
{
    return b ? int32_t(int64_t(a) / int64_t(b)) : 0;
}

// Returns a released voice's volume for the frame: its volume in the last frame, times (release + 0xE6) / 512.
[[maybe_unused]] uint32_t ReleaseFade(const Voice& v)
{
    return ((uint32_t(v.release) + 0xE6) * v.volume) >> 9;
}

// Returns an event without a note's settings.
[[maybe_unused]] Event MakeEvent(Event::Kind kind, int track, int voice, uint32_t id, uint64_t units, uint32_t value,
                                 Event::Phase phase = Event::kCommands)
{
    Event e;
    e.kind = kind;
    e.phase = phase;
    e.track = track;
    e.voice = voice;
    e.id = id;
    e.units = units;
    e.value = value;

    return e;
}

} // namespace

} // namespace agblib::rd2
