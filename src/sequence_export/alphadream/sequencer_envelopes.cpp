// SPDX-License-Identifier: MIT

// The envelopes and the PSG's channels of the model of AlphaDream's driver: the mixer's envelopes and positions, the
// envelopes of the squares and the noise, and the wave. Each routine's address in A88E is given where the model follows
// it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/alphadream/driver.h"
#include "agblib/sequence_export/alphadream/sequencer.h"
#include "sequencer_internal.h"

namespace agblib::sequence_export::alphadream
{

namespace
{

// The hardware's volume that the driver works out for a PSG channel from the envelope setting it last kept and the
// frames since (0x0819AC14). The hardware moves the volume by 1 every `step` 64ths of a second. The driver counts
// `step` frames for each move. Its division (0x081DAAA4) gives 0 for a step of 0.
int DriverVolume(const PsgChannel& c)
{
    const int start = c.written >> 4;
    if (!(c.written & 0xF))
    {
        return start;
    }

    const int step = c.written & 7;
    const int moved = step ? c.count / step : 0;
    return (c.written & 8) ? std::min(start + moved, 15) : std::max(start - moved, 0);
}

// Returns the hardware envelope's step that an envelope byte gives: the 64ths of a second for each move of the volume,
// or 0 for none.
uint8_t EnvelopeStep(uint8_t byte)
{
    return uint8_t((0xFF - byte) >> 5);
}

} // namespace

// The mixer's envelopes and positions (0x0819A2F0, 0x0819A398).
void Sequencer::RunEnvelopes()
{
    for (SampleChannel& c : samples_)
    {
        uint8_t status = c.status;
        int level = c.level;
        if (status == 0)
        {
            continue;
        }

        if (status == 0x80)
        {
            status = 0x81;
            level = c.envelope[0];
        }
        else if (status == 0x81)
        {
            level += c.envelope[0];
            if (level >= 0xFF)
            {
                level = 0xFF;
                status = 0x82;
            }
        }
        else if (status == 0x82)
        {
            // The driver compares the status, not the level, with the sustain level. A level at or above the decay's
            // step goes to the sustain level at once.
            if (level >= c.envelope[1] || status <= c.envelope[2])
            {
                status = 0x83;
                level = c.envelope[2];
            }
            else
            {
                level = (level - c.envelope[1]) & 0xFF;
            }
        }
        else if (status != 0x83)
        {
            if (level > c.envelope[3])
            {
                level -= c.envelope[3];
            }
            else
            {
                c.status = 0;
                continue;
            }
        }

        c.status = status;
        c.level = uint8_t(level);

        // The position. It ends a sample without a loop.
        SampleHeader s;
        if (!ReadSample(rom_, info_, c.sample & 0x7FFF, s))
        {
            c.status = 0;
            continue;
        }

        const bool fixed = c.sample & 0x8000;
        const int key = fixed ? kRootKey : c.pitch >> 8;
        const int fraction = fixed ? 0 : c.pitch & 0xFF;
        const uint8_t code = rom_.U8(info_.key_table + uint32_t(key));
        const uint32_t note = code & 0xF;
        const uint32_t a = rom_.U32(info_.step_table + 4 * note);
        const uint32_t b = rom_.U32(info_.step_table + 4 * note + 4);
        const uint32_t m = (a + uint32_t(((b - a) * uint32_t(fraction)) >> 8)) >> (code >> 4);
        const uint32_t high = uint32_t((uint64_t(s.rate) * m) >> 32);
        const uint32_t step = high + uint32_t((uint64_t(high) * kStepScale) >> 32);

        const uint32_t end = s.end << kPositionShift;
        uint32_t left = kFramePoints;
        for (int guard = 0; guard < 4096 && left; guard++)
        {
            const uint32_t to_end = end - c.position + step - 1;
            uint32_t n = left;
            if (uint64_t(step) * left >= to_end && step)
            {
                n = to_end / step;
            }

            left -= n;
            c.position += n * step;
            if (int32_t(c.position) < int32_t(end))
            {
                break;
            }

            if (!s.loops)
            {
                c.status = 0;
                break;
            }

            c.position += (s.loop_start - s.end) << kPositionShift;
        }
    }
}

// Runs a square's or the noise's envelope for a frame (0x0819AC84). The driver works the hardware's envelope in turns:
// a rise from 0 to the channel's volume, a fall to the sustain level, the sustain, and a fall to 0 after the note's
// end. Each turn restarts the channel with a new envelope setting, once the volume that the driver works out reaches
// the turn's end. In between, the frequency goes to the hardware each frame.
void Sequencer::RunPsgEnvelope(int channel)
{
    PsgChannel& c = psg_[size_t(channel)];
    uint16_t& control = registers_.control[size_t(channel)];
    uint16_t& frequency = registers_.frequency[size_t(channel)];

    // A84J's driver counts the frames whether or not it has written a setting (0x0814A844).
    if ((c.written || info_.version == Version::kHamtaro4) && c.count != 0xFF)
    {
        c.count++;
    }

    // Sets the hardware's envelope, keeps it in the record, and restarts the channel. The squares take their duty
    // again with the first two turns.
    const auto restart = [&](uint8_t setting, bool duty)
    {
        c.count = 0;
        c.written = setting;
        control = uint16_t(setting << 8 | (duty && channel != kNoiseChannel ? c.duty << 6 : control & 0xFF));
        frequency = uint16_t(c.frequency | 0x8000);
        registers_.restarted |= uint8_t(1 << channel);
    };

    const auto play = [&]()
    {
        frequency = c.frequency;
    };

    const auto silence = [&](bool keep)
    {
        if (!keep)
        {
            c.written = 0;
        }

        control &= 0xFF;
        c.state = 0;
    };

    switch (c.state)
    {
    case 1:
        c.state = 2;
        if (c.envelope[0] != 0xFF)
        {
            restart(uint8_t(EnvelopeStep(c.envelope[0]) | 8), true);
            return;
        }
        break;

    case 2:
        if (DriverVolume(c) < c.volume)
        {
            play();
            return;
        }
        break;

    case 3:
        {
            const int sustain = (c.volume * c.envelope[2]) >> 8;
            if (DriverVolume(c) > sustain)
            {
                play();
                return;
            }

            restart(uint8_t(sustain << 4), false);
            c.state = 4;
            return;
        }

    case 4:
        play();
        return;

    case 5:
        {
            // The release starts from the volume that the driver works out. Without a step, or from 0, the channel
            // goes silent at once, and the record keeps the old setting. A84J's driver checks only the step
            // (0x0814AA18): from 0, it restarts the channel at 0 with the release's step.
            const int volume = DriverVolume(c);
            c.count = 0;
            const uint8_t step = EnvelopeStep(c.envelope[3]);
            if (!step || (!volume && info_.version != Version::kHamtaro4))
            {
                silence(true);
                return;
            }

            restart(uint8_t(volume << 4 | step), false);
            c.state = 6;
            return;
        }

    case 6:
        if (DriverVolume(c))
        {
            play();
            return;
        }

        // A84J's driver keeps the setting in the record (0x0814AA78).
        silence(info_.version == Version::kHamtaro4);
        return;

    default:
        return;
    }

    // The fall from the channel's volume, after the rise or at once.
    restart(uint8_t(EnvelopeStep(c.envelope[1]) | c.volume << 4), true);
    c.state = 3;
}

// Runs the wave for a frame (0x0819AF18). A note's start restarts the wave at the NR32 setting for the channel's
// volume over 4, and the note's end mutes it. A change of the channel's volume reaches the hardware with the next note.
// The record of the wave keeps NR32 in the high byte as SOUND3CNT_H does.
void Sequencer::RunWave()
{
    PsgChannel& c = psg_[kWaveChannel];
    uint16_t& control = registers_.control[kWaveChannel];
    uint16_t& frequency = registers_.frequency[kWaveChannel];
    switch (c.state)
    {
    case 1:
        frequency = uint16_t(c.frequency | 0x8000);
        control = uint16_t(rom_.U8(info_.wave_levels + uint32_t(c.volume >> 2)) << 8);
        c.state = 2;
        break;

    case 2:
        frequency = c.frequency;
        break;

    case 5:
        control &= 0xFF;
        c.state = 0;
        break;

    default:
        break;
    }
}

// The PSG's output for a frame (0x0819AF88): the squares' and the noise's envelopes, then the hardware's response,
// from a model of the PSG. The wave's routine runs between the squares and the noise.
void Sequencer::RunPsg()
{
    registers_.restarted = 0;
    RunWave();
    for (int channel : {0, 1, kNoiseChannel})
    {
        RunPsgEnvelope(channel);
        const PsgRegisters r{0, registers_.control[size_t(channel)], registers_.frequency[size_t(channel)], 0,
                             (registers_.restarted & (1 << channel)) != 0};
        PsgModel& model = psg_models_[size_t(channel)];
        const int start = model.Frame(r);
        psg_volumes_[size_t(channel)] = start;

        // The frame's level is the mean of the volumes at its start and its end. The volume at the end is the one
        // that the next frame would start with if the driver wrote nothing. The envelope moves within the frame, and
        // the level of the frame's start would lag it by half a frame.
        PsgModel ahead = model;
        const int end = ahead.Frame(PsgRegisters{0, r.envelope, uint16_t(r.frequency & 0x7FFF), 0, false});
        psg_levels_[size_t(channel)] = start < 0 ? 0 : (start + std::max(end, 0)) / 2.0;
    }
}

// Returns true while a square or the noise sounds: on, at a volume above 0 or with an envelope that rises from 0.
bool Sequencer::PsgSounds(int channel) const
{
    const int volume = psg_volumes_[size_t(channel)];
    const uint16_t envelope = registers_.control[size_t(channel)] >> 8;
    return volume > 0 || (volume == 0 && (envelope & 8) && (envelope & 7));
}

} // namespace agblib::sequence_export::alphadream
