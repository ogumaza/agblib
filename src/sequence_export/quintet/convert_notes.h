// SPDX-License-Identifier: MIT

// The notes of a Quintet song's channels, and their instruments. Only convert.cpp includes this header. Its definitions
// therefore keep internal linkage.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "convert_internal.h"

namespace agblib::sequence_export::quintet
{
namespace
{

// Returns the registers of PSG channel `c` after a frame, for its model.
PsgRegisters PsgRegistersOf(const Hardware& hw, int c)
{
    return {hw.registers[kNr10], hw.registers[size_t(kPsgEnvelope[c])], hw.registers[size_t(kPsgControl[c])],
            hw.registers[kNr30], (hw.restarted & (1 << c)) != 0};
}

// Returns the wave shape that the wave channel plays after a frame: the bank NR30 selects, or both banks starting with
// it in the 64-step mode.
WaveShape WaveShapeOf(const Hardware& hw)
{
    const uint16_t nr30 = hw.registers[kNr30];
    const size_t bank = (nr30 >> 6) & 1;
    WaveShape shape;
    for (size_t b = 0; b < ((nr30 & 0x20) ? 2u : 1u); b++)
    {
        for (uint8_t v : hw.wave_ram[(bank + b) & 1])
        {
            shape.steps.push_back(uint8_t(v >> 4));
            shape.steps.push_back(uint8_t(v & 15));
        }
    }

    return shape;
}

// Returns how many keys above a square note on the same entry of the frequency table a wave note plays with `shape`. A
// wave pattern of 32 steps plays an octave lower than a square, one of 64 two octaves lower, and one that repeats
// sounds higher.
int WaveKeyShift(const WaveShape& shape)
{
    return WaveKeyOffset(shape) - (shape.steps.size() > 32 ? 24 : 12);
}

NoteMaker::NoteMaker(const Rom& rom, const DriverInfo& info, const Simulation& sim)
    : rom_(rom), info_(info), sim_(sim), end_(sim.plan.end)
{
}

std::vector<Note> NoteMaker::Make(int c)
{
    // Each note or "play again" starts a stretch of the channel. The stretch lasts until its next note, rest or
    // end.
    std::vector<size_t> starts;
    for (size_t i = 0; i < sim_.events.size(); i++)
    {
        const Event& e = sim_.events[i];
        if (e.channel == c &&
            (e.kind == Event::kNote || e.kind == Event::kPlayAgain || e.kind == Event::kRest || e.kind == Event::kEnd))
        {
            starts.push_back(i);
        }
    }

    // A PSG channel's sound comes from a model of the channel.
    std::vector<Note> notes;
    std::optional<PsgFeed> psg;
    if (c < kPcmA)
    {
        psg.emplace(c, [this, c](uint32_t f) { return PsgRegistersOf(sim_.frames[f].hardware, c); });
    }

    // A "play again" keeps the key of the note before. On a PCM channel, a "play again" plays the sample at the
    // sample's rate on the root key.
    int key = 60;
    int base = 0; // the frequency setting or PCM pitch that the key stands for
    for (size_t i = 0; i < starts.size(); i++)
    {
        const Event& e = sim_.events[starts[i]];
        Stretch stretch;
        stretch.first = e.frame;
        stretch.last = i + 1 < starts.size() ? sim_.events[starts[i + 1]].frame : uint32_t(sim_.frames.size());
        stretch.on = sim_.event_ticks[starts[i]];
        stretch.stop = std::min(i + 1 < starts.size() ? sim_.event_ticks[starts[i + 1]] : end_, end_);
        if (e.kind == Event::kRest || e.kind == Event::kEnd || stretch.on >= stretch.stop)
        {
            continue;
        }

        if (e.kind == Event::kNote)
        {
            key = KeyOf(c, e.index, base);
        }
        else if (c >= kPcmA)
        {
            key = kPcmRootKey;
            base = 0;
        }

        stretch.key = key;
        stretch.base = base;
        stretch.command = e.address;
        AddStretch(c, stretch, psg ? &*psg : nullptr, notes);
    }

    return notes;
}

void NoteMaker::AddStretch(int c, const Stretch& stretch, PsgFeed* psg, std::vector<Note>& notes)
{
    Note note;
    bool open = false;
    for (uint32_t f = stretch.first; f < stretch.last && f < sim_.frames.size(); f++)
    {
        const FrameData& d = sim_.frames[f];
        const uint32_t into = stretch.on - sim_.frame_ticks[stretch.first];
        const uint32_t tick = std::min({sim_.frame_ticks[f] + into, sim_.frame_ticks[f + 1] - 1, stretch.stop - 1});
        Sound s = psg ? PsgSound(c, psg->Volume(f), psg->Model(), d, stretch.base) : FifoSound(c, d, stretch.base);
        s.tick = tick;

        // The sweep's steps go as far into the frame's ticks as they come into the frame. They stay before the next
        // frame's changes.
        const uint32_t length = sim_.frame_ticks[f + 1] - sim_.frame_ticks[f];
        const uint32_t last_tick = std::min(sim_.frame_ticks[f + 1] + into, stretch.stop) - 1;
        for (SweepStep& step : s.sweeps)
        {
            step.tick = std::min(tick + uint32_t(std::lround(step.at * length)), last_tick);
        }

        if (!s.sounding)
        {
            if (open)
            {
                note.off = tick;
                Finish(c, note, notes);
                open = false;
            }

            continue;
        }

        if (!open)
        {
            note = Note();
            note.key = stretch.key;
            note.on = tick;
            note.command = stretch.command;
            if (c >= kPcmA)
            {
                note.pcm = PcmSoundOf(d.fifos[size_t(c - kPcmA)]);
            }

            open = note.on < stretch.stop;
        }

        if (open)
        {
            note.sounds.push_back(s);
            if (c == kNoise)
            {
                note.noise.AddFrame(d.hardware.registers[kNr41], d.hardware.registers[kNr43],
                                    (d.hardware.restarted & (1 << kNoise)) != 0);
            }
        }
    }

    if (open)
    {
        note.off = stretch.stop;
        Finish(c, note, notes);
    }
}

int NoteMaker::KeyOf(int c, int index, int& base) const
{
    if (c >= kPcmA)
    {
        base = int16_t(rom_.U16(info_.pcm_pitch_table + uint32_t(index) * 2));
        return kPcmRootKey + index - kPcmUnityIndex;
    }

    base = rom_.U16(info_.frequency_table + uint32_t(index) * 2) & 0x7FF;

    return kSquareBaseKey + index;
}

Sound NoteMaker::PsgSound(int c, int volume, const PsgModel& psg, const FrameData& d, int base)
{
    Sound s;
    const Hardware& hw = d.hardware;
    if (volume < 0)
    {
        return s;
    }

    const PsgScale scale = PsgScaleOf(hw.registers[kNr50], hw.registers[kSoundcntH]);
    const int nr51 = hw.registers[kNr50] >> 8;
    double level = 0;
    switch (c)
    {
    case kWave:
        // NR32 scales the wave.
        level = 2 * kPsgLevelPerVolume * 15 * WaveShare(hw.registers[kNr31]);
        s.shape = shapes_.Number(WaveShapeOf(hw));
        break;

    case kNoise:
        // The drum's sample has the noise channel's volume in it.
        level = kPsgLevelPerVolume * 15;
        break;

    default:
        level = kPsgLevelPerVolume * volume;
        s.shape = (hw.registers[size_t(kPsgEnvelope[c])] >> 6) & 3;
        s.envelope = (hw.registers[size_t(kPsgEnvelope[c])] >> 8) | (((hw.restarted >> c) & 1) << 8);
        s.sweep = c == 0 ? hw.registers[kNr10] : -1;
        break;
    }

    s.sounding = true;
    s.side_left = (nr51 & (0x10 << c)) ? scale.left : 0;
    s.side_right = (nr51 & (0x01 << c)) ? scale.right : 0;
    s.left = level * s.side_left;
    s.right = level * s.side_right;

    if (c != kNoise && base)
    {
        s.bend = PsgSemitones(base, psg.Frequency());
        for (const auto& [at, frequency] : psg.Sweeps())
        {
            s.sweeps.push_back({at, 0, PsgSemitones(base, frequency)});
        }
    }

    return s;
}

Sound NoteMaker::FifoSound(int c, const FrameData& d, int base) const
{
    Sound s;
    const int f = c - kPcmA;
    const Fifo& fifo = d.fifos[size_t(f)];
    if (fifo.position == ~0u || !fifo.base_rate || !fifo.rate)
    {
        return s;
    }

    const uint16_t h = d.hardware.registers[kSoundcntH];
    const double level = (h & kFifoVolume[f]) ? kFifoLevel : kFifoLevel / 2;
    s.sounding = true;
    s.left = (h & kFifoLeft[f]) ? level : 0;
    s.right = (h & kFifoRight[f]) ? level : 0;

    // The key plays at the rate that the note's pitch stands for. A detune or an effect can make the note play at
    // another rate.
    const double nominal = fifo.base_rate + double(fifo.base_rate) * base / 1000.0;
    s.bend = 12 * std::log2(double(fifo.rate) / nominal);

    return s;
}

PcmSound NoteMaker::PcmSoundOf(const Fifo& fifo)
{
    PcmSound p;
    p.start = fifo.sample;
    p.from = fifo.next_start - fifo.sample;
    p.looped = fifo.loop != 0;
    p.loop = p.looped ? fifo.loop - fifo.sample : 0;
    p.rate = fifo.base_rate;

    const double bytes = double(kFrameCycles) / std::max<uint32_t>(SampleCycles(fifo.rate), 1);
    const bool in_loop = p.looped && fifo.next_start == fifo.loop;
    const uint32_t first = in_loop ? 0 : FifoFrames(p.from * 60, fifo.rate, fifo.end);
    const uint32_t pass = FifoFrames(fifo.loop_position + fifo.rate, fifo.rate, fifo.end) + 1;
    p.first = uint32_t(std::lround(first * bytes));
    p.loop_length = p.looped ? uint32_t(std::lround(pass * bytes)) : 0;
    p.from = in_loop ? p.loop : p.from;

    return p;
}

void NoteMaker::Finish(int c, Note& note, std::vector<Note>& notes)
{
    if (note.sounds.empty() || note.on >= note.off)
    {
        return;
    }

    Instrument inst;
    if (c == kNoise)
    {
        note.noise.Trim();
        inst.kind = Instrument::kNoiseKit;
        note.key = drums_.try_emplace(note.noise, kFirstDrumKey + int(drums_.size())).first->second;
    }
    else if (c >= kPcmA)
    {
        inst.kind = Instrument::kPcm;
        inst.pcm = note.pcm;
    }
    else
    {
        // The shape at the start, and the first one it switches to.
        inst.kind = c == kWave ? Instrument::kWave : Instrument::kSquare;
        inst.first = note.sounds[0].shape;
        inst.second = inst.first;
        for (size_t i = 1; i < note.sounds.size(); i++)
        {
            if (note.sounds[i].shape != inst.first)
            {
                inst.frames = int(i);
                inst.second = note.sounds[i].shape;
                break;
            }
        }

        if (c == kWave)
        {
            note.key += WaveKeyShift(shapes_[inst.first]);
        }
    }

    note.program = programs_.try_emplace(inst, int(programs_.size())).first->second;
    notes.push_back(note);
}

} // namespace
} // namespace agblib::sequence_export::quintet
