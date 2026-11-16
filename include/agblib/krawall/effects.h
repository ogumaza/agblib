// SPDX-License-Identifier: MIT

// The numbers of Krawall's effects in the effect column of a module's patterns. A kOther item of a pattern
// (`agblib/krawall/commands.h`) contains kEffectOpcode plus one of these numbers in `encoding.opcode`.

#pragma once

namespace agblib::krawall
{

// The effects of the effect column, as the module's patterns number them.
enum EffectNumber : int
{
    kSpeed = 1,
    kBpm = 2,
    kSpeedBpm = 3,
    kPatternJump = 4,
    kPatternBreak = 5,
    kVolumeSlideS3m = 6,
    kVolumeSlideXm = 7,
    kVolumeSlideDownFine = 8,
    kVolumeSlideUpFine = 9,
    kPortaDownXm = 10,
    kPortaDownS3m = 11,
    kPortaDownFine = 12,
    kPortaDownExtraFine = 13,
    kPortaUpXm = 14,
    kPortaUpS3m = 15,
    kPortaUpFine = 16,
    kPortaUpExtraFine = 17,
    kVolume = 18,
    kPortaNote = 19,
    kVibrato = 20,
    kTremor = 21,
    kArpeggio = 22,
    kVolumeSlideVibrato = 23,
    kVolumeSlidePorta = 24,
    kChannelVolume = 25,
    kChannelVolumeSlide = 26,
    kOffset = 27,
    kPanSlide = 28,
    kRetrig = 29,
    kTremolo = 30,
    kFineVibrato = 31,
    kGlobalVolume = 32,
    kGlobalVolumeSlide = 33,
    kPan = 34,
    kPanbrello = 35,
    kMark = 36,
    kGlissando = 37,
    kVibratoWave = 38,
    kTremoloWave = 39,
    kPanbrelloWave = 40,
    kPatternLoop = 43,
    kNoteCut = 44,
    kNoteDelay = 45,
    kVolumeSlideVibratoXm = 49,
    kVolumeSlidePortaXm = 50,
    kEffectCount = 51,
};

} // namespace agblib::krawall
