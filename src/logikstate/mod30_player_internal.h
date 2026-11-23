// SPDX-License-Identifier: MIT

// Definitions that the parts of the player of the MOD-30 driver share (mod30_player.cpp and
// mod30_player_effects.cpp): the constants of the driver, and its BIOS arithmetic. Only those files include this
// header. The definitions keep internal linkage.

#pragma once

#include <array>
#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/logikstate/mod30.h"

namespace agblib::logikstate
{
namespace
{

// The first half of the vibrato's wave: ProTracker's sine table. The second half is the first, negated.
constexpr std::array<int32_t, 32> kSine = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
                                           224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
                                           212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

// The Amiga's clock that the drivers divide by twice a period.
constexpr int32_t kPeriodClock = 0x6C3E1D;

// The highest linear period, and the periods of an octave.
constexpr int32_t kLinearTop = 7680;
constexpr int32_t kOctave = 768;

// The linear period of note 1 at no relative note and no fine tune, and the period of a semitone.
constexpr int32_t kLinearBase = 7616;
constexpr int kSemitoneShift = 6;

// A position's fraction: 4096ths of a point.
constexpr int kFractionBits = 12;

// The fade's full value, and what the frame routine adds to it each frame.
constexpr uint32_t kFullFade = 0x40;
constexpr uint32_t kFadeStep = 3;

// A channel's volume at most, and the length up to which the mixer leaves a channel alone.
constexpr int32_t kFullVolume = 64;
constexpr uint32_t kStoppedLength = 2;

// The channels of the first builds' ticks, and the record whose volume column those ticks apply.
constexpr int kTickChannels = 4;

// The largest operand of effect F that sets the speed. A larger one sets the tempo.
constexpr uint8_t kLargestSpeed = 0x1F;

// The effects.
constexpr uint8_t kArpeggio = 0x0;
constexpr uint8_t kSlideUp = 0x1;
constexpr uint8_t kSlideDown = 0x2;
constexpr uint8_t kSlideToNote = 0x3;
constexpr uint8_t kVibrato = 0x4;
constexpr uint8_t kSlideToNoteAndVolume = 0x5;
constexpr uint8_t kVibratoAndVolume = 0x6;
constexpr uint8_t kVolumeSlide = 0xA;
constexpr uint8_t kJump = 0xB;
constexpr uint8_t kVolume = 0xC;
constexpr uint8_t kBreak = 0xD;
constexpr uint8_t kExtended = 0xE;
constexpr uint8_t kSpeed = 0xF;
constexpr uint8_t kPatternLoop = 0x60;

// The linear slides' fine and extra-fine forms, in the operand's high 4 bits.
constexpr uint32_t kFine = 0xF;
constexpr uint32_t kExtraFine = 0xE;

// The most passes of a frame loop. A loop that makes no progress stops there.
constexpr int kMaxPasses = 100000;

// Divides as the BIOS does: signed, truncating towards 0. A divisor of 0 gives 0.
int32_t Divide(int32_t value, int32_t by)
{
    return by ? int32_t(int64_t(value) / by) : 0;
}

int32_t Remainder(int32_t value, int32_t by)
{
    return by ? int32_t(int64_t(value) % by) : value;
}

} // namespace
} // namespace agblib::logikstate
