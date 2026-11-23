// SPDX-License-Identifier: MIT

// The PSG's part of the model of Ubisoft Milan's driver: a note's start on a square or the wave channel, its legato and
// its end, and the frame's update of the modulation and the glides.

#include <array>
#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/agb/sound_registers.h"
#include "agblib/ubimilan/driver.h"
#include "agblib/ubimilan/sequencer.h"
#include "sequencer_internal.h"

namespace agblib::ubimilan
{

namespace
{

// Divides as the game's library does: towards 0, with 0 for a divisor of 0.
int32_t Divide(int32_t a, int32_t b)
{
    return b ? int32_t(int64_t(a) / int64_t(b)) : 0;
}

} // namespace

void Sequencer::PsgNoteOn(int channel, int key, int velocity)
{
    if (!info_.instrument_tables[size_t(channel)] || !info_.frequency_table)
    {
        return;
    }

    // A note continues from the one before if the channel plays and the instrument has legato. Otherwise it starts the
    // channel. Either way the channel stores the new key, and the next note-off has to match it.
    PsgChannel& psg = psg_[size_t(channel)];
    const int index = KeyIndex(channel, key);
    Note n = NewNote(channel, key, velocity, psg.program);
    const uint32_t flags = info_.instrument_tables[0] + 16 * uint32_t(psg.program + 30 * channel) + kInstrumentFlags;
    if (psg.playing && (rom_.U16(flags) & kLegato))
    {
        PsgLegato(channel, index);
        n.legato = true;
        if (psg.note >= 0)
        {
            n.duty = notes_[size_t(psg.note)].duty;
            n.envelope = notes_[size_t(psg.note)].envelope;
        }
    }
    else
    {
        PsgStart(channel, index, velocity);
        psg.playing = true;
        const uint32_t inst = Instrument(channel, psg.program);
        n.duty = channel == 0 ? rom_.U8(inst + 1) : channel == 1 ? rom_.U8(inst) : 0;
        n.envelope = uint8_t(velocity | (channel == 0 ? rom_.U8(inst + 2) : channel == 1 ? rom_.U8(inst + 1) : 0));
    }

    psg.index = index;

    if (psg.note >= 0)
    {
        EndNote(psg.note, frame_);
    }

    psg.note = int(notes_.size());
    notes_.push_back(n);
}

uint32_t Sequencer::Instrument(int channel, int program) const
{
    return info_.instrument_tables[size_t(channel)] + 16 * uint32_t(program & 0xFF);
}

uint16_t Sequencer::Frequency(int index) const
{
    return rom_.U16(info_.frequency_table + 2 * uint32_t(index));
}

uint16_t Sequencer::FineFrequency(int32_t cents) const
{
    return info_.fine_table ? rom_.U16(info_.fine_table + 2 * uint32_t(cents)) : 0;
}

void Sequencer::PsgStart(int channel, int index, int velocity)
{
    // The instrument's bytes and the velocity. The velocity is ORed into the envelope. The wave channel's DAC comes on,
    // and its velocity is its volume. The last byte for the channel goes above the frequency setting, in NRx4.
    PsgChannel& psg = psg_[size_t(channel)];
    const uint32_t inst = Instrument(channel, psg.program);
    uint32_t frequency_register = 0;
    uint32_t high = 0;
    if (channel == 0)
    {
        Write(agb::kRegNr10, 2, rom_.U8(inst));
        Write(agb::kRegNr11, 2, rom_.U8(inst + 1) | (uint32_t(velocity | rom_.U8(inst + 2)) << 8));
        frequency_register = agb::kRegNr13;
        high = uint32_t(rom_.U8(inst + 3)) << 8;
    }
    else if (channel == 1)
    {
        Write(agb::kRegNr21, 2, rom_.U8(inst) | (uint32_t(velocity | rom_.U8(inst + 1)) << 8));
        frequency_register = agb::kRegNr23;
        high = uint32_t(rom_.U8(inst + 2)) << 8;
    }
    else
    {
        Write(agb::kRegNr30, 2, 0xA0);
        Write(agb::kRegNr31, 2, rom_.U8(inst) | (uint32_t(velocity) << 8));
        frequency_register = agb::kRegNr33;
        high = uint32_t(rom_.U8(inst + 1)) << 8;
    }

    // The modulation starts with its delay. Without the fine table, the model leaves out the modulation and the pitch
    // mode. Detection warns about that.
    const uint16_t kept = info_.fine_table ? 0xFFFF : uint16_t(~(kModulation | kPitchMode));
    const uint16_t flags = rom_.U16(inst + kInstrumentFlags) & kept;
    Modulation& mod = psg.modulation;
    if (flags & kModulation)
    {
        mod.value = rom_.U16(inst + kModulationDelay);
        mod.step = Divide(int32_t(uint32_t(rom_.U16(inst + kModulationDepth)) << 16),
                          int32_t(rom_.U16(inst + kModulationPeriod)) * 2);
        mod.depth = rom_.U16(inst + kModulationDepth);
        mod.state = 1;
    }
    else
    {
        mod.state = 0;
    }

    // The pitch mode's first note starts at its key. A later one glides there from the note before, or from where a
    // glide has reached.
    PitchMode& pitch = psg.pitch;
    const int32_t target = 100 * index;
    if (!(flags & kPitchMode))
    {
        Write(frequency_register, 2, (high | Frequency(index)) & 0xFFFF);
        pitch.state = 0;
        return;
    }

    switch (pitch.state)
    {
    case 0:
        Write(frequency_register, 2, (high | Frequency(index)) & 0xFFFF);
        pitch.frames = rom_.U16(inst + kSlideFrames);
        pitch.state = 1;
        pitch.previous = pitch.current = pitch.target = target;
        break;

    case 1:
        Write(frequency_register, 2, (high | Frequency(psg.index)) & 0xFFFF);
        Glide(pitch, inst, target);
        break;

    case 2:
    case 3:
        Write(frequency_register, 2, (high | FineFrequency(pitch.current)) & 0xFFFF);
        Glide(pitch, inst, target);
        break;

    default:
        break;
    }
}

void Sequencer::Glide(PitchMode& pitch, uint32_t inst, int32_t target) const
{
    // A glide to the note's place in cents, over the instrument's frames. A glide to the place of the last target
    // keeps its step at 0. The pitch then stays where it is unless it's below the target.
    if (pitch.state == 1)
    {
        pitch.state = 2;
    }
    else
    {
        pitch.previous = pitch.target;
    }

    pitch.target = target;
    if (target == pitch.previous)
    {
        pitch.step = 0;
        return;
    }

    pitch.frames = rom_.U16(inst + kSlideFrames);
    pitch.step = Divide(target - pitch.current, pitch.frames);
    pitch.state = 2;
}

void Sequencer::PsgLegato(int channel, int index)
{
    // A legato note writes only the frequency setting of its key, without a restart, or glides there in the pitch
    // mode. On the wave channel, the pitch-mode check reads all the flags. A wave instrument with legato therefore
    // always uses the glide code. Without a glide state, the frequency stays.
    PsgChannel& psg = psg_[size_t(channel)];
    const uint32_t inst = Instrument(channel, psg.program);
    const uint16_t flags =
        rom_.U16(info_.instrument_tables[0] + 16 * uint32_t(psg.program + 30 * channel) + kInstrumentFlags);
    const bool pitch_mode = channel == kWaveChannel ? flags != 0 : (flags & kPitchMode) != 0;
    if (!pitch_mode)
    {
        Write(channel == 0 ? agb::kRegNr13 : channel == 1 ? agb::kRegNr23 : agb::kRegNr33, 2, Frequency(index));
    }
    else if (psg.pitch.state >= 1 && psg.pitch.state <= 3)
    {
        Glide(psg.pitch, inst, 100 * index);
    }
}

void Sequencer::PsgNoteOff(int channel, int key)
{
    // A note-off silences the channel if its key is the one that the last note set. GCC warns that a negative channel
    // would index past psg_. at() checks the index.
    PsgChannel& psg = psg_.at(size_t(channel));
    if (psg.index != KeyIndex(channel, key))
    {
        return;
    }

    psg.playing = false;
    Write(channel == 0 ? agb::kRegNr11 : channel == 1 ? agb::kRegNr21 : agb::kRegNr31, 2, 0);
    if (psg.note >= 0)
    {
        EndNote(psg.note, frame_);
        psg.note = -1;
    }
}

void Sequencer::PsgUpdate()
{
    // Each frame after the commands, the sequencer moves the modulation and the glide of each PSG channel on. If
    // either sets a place in cents, the channel gets the frequency setting at that place in the fine table, without a
    // restart. The modulation alone moves the place of the channel key. The modulation and the glide continue after a
    // note-off.
    for (int c = 0; c < kPsgChannels; c++)
    {
        PsgChannel& psg = psg_[size_t(c)];
        int32_t mod = 0;
        int32_t glide = 0;
        Modulation& m = psg.modulation;
        if (m.state)
        {
            switch (m.state)
            {
            case 1:
                if (m.value == 0)
                {
                    m.state = 2;
                }
                else
                {
                    m.value--;
                }
                break;

            case 2:
            case 3:
                m.value = int32_t(uint32_t(m.value) + uint32_t(m.step));
                if ((m.value >> 16) > m.depth)
                {
                    m.state = 4;
                }
                break;

            case 4:
                m.value = int32_t(uint32_t(m.value) - uint32_t(m.step));
                if (int64_t(m.value >> 16) + m.depth < 0)
                {
                    m.state = 3;
                }
                break;

            default:
                break;
            }

            mod = m.state == 1 ? 0 : int16_t(uint32_t(m.value) >> 16);
        }

        PitchMode& p = psg.pitch;
        if (p.state)
        {
            if (p.state == 2)
            {
                const bool reached = p.step > 0 ? p.current >= p.target : p.current <= p.target;
                if (reached)
                {
                    p.current = p.target;
                    p.state = 3;
                }
                else
                {
                    p.current += p.step;
                }
            }

            glide = p.current;
        }

        if (!mod && !glide)
        {
            continue;
        }

        const int32_t cents = glide + mod + (glide ? 0 : 100 * psg.index);
        Write(kFrequencyRegisters[size_t(c)], 2, FineFrequency(cents));
    }
}

} // namespace agblib::ubimilan
