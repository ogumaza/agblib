// SPDX-License-Identifier: MIT

// The simulation of a Quintet song and the MIDI file's timing. Only convert.cpp includes this header. Its definitions
// therefore keep internal linkage.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "convert_internal.h"

namespace agblib::sequence_export::quintet
{
namespace
{

// Returns the MIDI file's tempo in a frame: the first channel's tempo T, in which the frame lasts T ticks.
int MidiTempo(const FrameData& d)
{
    return std::max<int>(d.tempo[0], kMinMidiTempo);
}

// Returns each event's MIDI tick at the start of the frame that the driver plays it in.
std::vector<uint32_t> FrameStartTicks(const Simulation& sim)
{
    std::vector<uint32_t> ticks(sim.events.size());
    for (size_t i = 0; i < sim.events.size(); i++)
    {
        ticks[i] = sim.frame_ticks[sim.events[i].frame];
    }

    return ticks;
}

// Returns each event's MIDI tick on the song's beat. The driver times a channel's notes by counting its ticks from the
// last time it reset the count: at the song's start, at a loop and at a tempo change. It plays each note at the start
// of the frame that its count reaches. So the first note after a reset starts its frame. The notes after it keep to
// their ticks within the frames that the driver plays them in. A tick lasts 37 MIDI ticks at the first channel's tempo.
std::vector<uint32_t> BeatTicks(const Simulation& sim)
{
    std::vector<uint32_t> ticks(sim.events.size());
    std::array<bool, kChannels> reset;
    reset.fill(true);
    std::array<uint32_t, kChannels> base = {};
    std::array<uint64_t, kChannels> base_tick = {};
    std::array<double, kChannels> scale = {};
    for (size_t i = 0; i < sim.events.size(); i++)
    {
        const Event& e = sim.events[i];
        const size_t c = e.channel;
        const uint32_t start = sim.frame_ticks[e.frame];
        const uint32_t next = sim.frame_ticks[e.frame + 1];

        // A loop resets the channel's count, and so does a tempo: on the first channel, every channel's. A loop point
        // read before the first note after a reset belongs to that note's frame.
        if (e.kind == Event::kTempo || e.kind == Event::kLoop)
        {
            if (e.kind == Event::kTempo && c == 0)
            {
                reset.fill(true);
            }

            reset[c] = true;
            ticks[i] = start;
            continue;
        }

        if (reset[c] && e.kind == Event::kLoopPoint)
        {
            ticks[i] = start;
            continue;
        }

        // The events of a performance keep to the channel's count, and don't start it after a reset. The first note
        // after a reset starts its frame, and so do they.
        if (e.kind == Event::kStretch || e.kind == Event::kVolume)
        {
            const double tick = base[c] + double(e.tick - base_tick[c]) * scale[c];
            ticks[i] = reset[c] ? start : uint32_t(std::clamp<double>(std::round(tick), start, next - 1));
            continue;
        }

        if (reset[c])
        {
            reset[c] = false;
            base[c] = start;
            base_tick[c] = e.tick;
            const FrameData& d = sim.frames[e.frame];
            scale[c] = double(kMidiTicksPerTick) * MidiTempo(d) / std::max<uint16_t>(d.tempo[c], 1);
        }

        const double tick = base[c] + double(e.tick - base_tick[c]) * scale[c];
        ticks[i] = uint32_t(std::clamp<double>(std::round(tick), start, next - 1));
    }

    return ticks;
}

// Returns the events that the song's loop waits for: each looping channel's loop point, and the end of each other
// channel that has ended or stopped at its loop point.
std::vector<int> LoopStartEvents(const std::array<ChannelLoop, kChannels>& channel_loops)
{
    std::vector<int> events;
    for (const ChannelLoop& loop : channel_loops)
    {
        if (loop.Loops())
        {
            events.push_back(loop.loop_point);
        }
        else if (loop.end >= 0)
        {
            events.push_back(loop.end);
        }
    }

    return events;
}

// Works out the song's loop from the events so far. Returns true once every channel has looped, ended or stopped at its
// loop point. Each call sets the loop channel: the channel with the longest loop, or -1. When a channel loops, the call
// that returns true also sets `times` and `run_on`. Channels may have different loop points and lengths. The song's
// loop starts once all looping channels have entered their loops and the others have ended or stopped. `times` is the
// number of passes of the longest loop that LoopLength() gives for the channels' loops.
bool TryPlan(Simulation& sim, uint32_t& run_on)
{
    bool planned = true;
    sim.loop_channel = -1;
    uint64_t longest = 0;
    std::vector<uint64_t> lengths;
    for (size_t c = 0; c < kChannels; c++)
    {
        const ChannelLoop& loop = sim.channel_loops[c];
        planned = planned && (loop.idle || !loop.passes.empty() || loop.end >= 0);
        const uint64_t length =
            loop.Loops() ? sim.events[size_t(loop.passes[0])].tick - sim.events[size_t(loop.loop_point)].tick : 0;
        if (length > 0)
        {
            lengths.push_back(length);
        }

        if (length > longest)
        {
            longest = length;
            sim.loop_channel = int(c);
        }
    }

    // The song runs on as much longer as the loop starts after the loop channel's loop point. It runs on for at least
    // as many MIDI ticks as the frames from that loop point to the latest took. The tempo while it runs on doesn't
    // change that. If they're in the same frame, it runs on to the end of that frame.
    if (planned && sim.loop_channel >= 0)
    {
        sim.times = size_t(LoopLength(lengths) / longest);
        const int loop_point = sim.channel_loops[size_t(sim.loop_channel)].loop_point;
        const uint32_t first = sim.events[size_t(loop_point)].frame;
        uint32_t latest = first;
        for (int e : LoopStartEvents(sim.channel_loops))
        {
            latest = std::max(latest, sim.events[size_t(e)].frame);
        }

        run_on = latest > first ? sim.frame_ticks[latest + 1] - sim.frame_ticks[first] : 0;
    }

    return planned;
}

// Runs the model until every channel has looped, ended or stopped at its loop point and the song has played its loop
// `loops` times. With `record`, the model records a performance too.
Simulation RunModel(const Rom& rom, const DriverInfo& info, int song, int loops, bool record = false)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    seq.RecordPerformance(record);
    for (int c = 0; c < kChannels; c++)
    {
        sim.channel_loops[size_t(c)].idle = !seq.GetChannel(c).position;
    }

    sim.frame_ticks.push_back(0);
    bool planned = false;
    bool finished = false; // the song played what the plan requires
    uint32_t run_on = 0;   // the MIDI ticks the model runs on for after the loop channel's last pass
    for (uint32_t f = 0; f < kMaxFrames && !seq.Ended(); f++)
    {
        seq.Step();
        FrameData d;
        d.hardware = seq.GetHardware();
        d.fifos = {seq.GetFifo(0), seq.GetFifo(1)};
        for (int c = 0; c < kChannels; c++)
        {
            d.tempo[size_t(c)] = seq.GetChannel(c).tempo;
        }

        sim.frames.push_back(d);

        // The MIDI file's tempo map follows the first channel. Every frame then keeps the same length.
        sim.frame_ticks.push_back(sim.frame_ticks.back() + uint32_t(MidiTempo(d)));

        for (const Event& e : seq.Events())
        {
            ChannelLoop& loop = sim.channel_loops[e.channel];
            const int index = int(sim.events.size());
            sim.events.push_back(e);
            if (e.kind == Event::kLoopPoint && loop.passes.empty())
            {
                loop.loop_point = index;
            }
            else if (e.kind == Event::kLoop)
            {
                loop.passes.push_back(index);
            }
            else if (e.kind == Event::kEnd || e.kind == Event::kHold)
            {
                loop.end = index;
            }
        }

        // Once every channel has looped, ended or stopped at its loop point, the loop is known. The song then continues
        // for `loops` passes through it.
        if (!planned)
        {
            planned = TryPlan(sim, run_on);
        }

        if (planned && sim.loop_channel < 0)
        {
            finished = true;
            break;
        }

        const size_t needed = size_t(loops) * sim.times; // the loop channel's passes in the song
        if (planned && sim.channel_loops[size_t(sim.loop_channel)].passes.size() >= needed)
        {
            const int last = sim.channel_loops[size_t(sim.loop_channel)].passes[needed - 1];
            if (sim.frame_ticks[f] >= sim.frame_ticks[sim.events[size_t(last)].frame] + run_on)
            {
                finished = true;
                break;
            }
        }
    }

    sim.warnings = seq.Warnings();
    sim.stretches = seq.Stretches();
    if (!finished && !seq.Ended() && sim.frames.size() == kMaxFrames)
    {
        sim.warnings.push_back("the song was cut off after an hour");
        sim.cut_off = true;
    }

    return sim;
}

// Times the events of `sim` for `timing`. Then plans the stretch of the song that the MIDI file covers: `loops` passes
// through the loop from `start_pass`. The loop starts where every looping channel has started its loop and every other
// channel has ended or stopped. The delay is how far that comes after the loop channel's loop point. The loop ends that
// delay after the end of the loop channel's `times`th pass. From the second pass, the loop starts and ends a loop
// later: that delay after the ends of its `times`th and twice `times`th passes. The song's next pass through the loop
// ends a loop after the loop's end. The song ends that delay after its last pass. If the song doesn't loop, it ends
// where the last channel ends or stops. A song that the model cut off ends where it stopped.
void PlanLoop(Simulation& sim, Timing timing, int loops, LoopStart start_pass)
{
    sim.event_ticks = timing == Timing::kFrames ? FrameStartTicks(sim) : BeatTicks(sim);
    sim.plan = Plan();
    Plan& plan = sim.plan;
    if (sim.loop_channel >= 0)
    {
        const ChannelLoop& loop = sim.channel_loops[size_t(sim.loop_channel)];
        const std::vector<int>& p = loop.passes;
        uint32_t start = sim.event_ticks[size_t(loop.loop_point)];
        for (int e : LoopStartEvents(sim.channel_loops))
        {
            start = std::max(start, sim.event_ticks[size_t(e)]);
        }

        const uint32_t first_pass = sim.event_ticks[size_t(p[0])] - sim.event_ticks[size_t(loop.loop_point)];
        const uint32_t delay = start - sim.event_ticks[size_t(loop.loop_point)];

        // Where the loop channel's `n`th pass ends, moved as far on as the loop starts after its loop point: the loop
        // start for 0, and if the model stopped before that pass ended, where it would at the first pass's length.
        const auto pass_end = [&](size_t n)
        {
            return n == 0          ? start
                   : p.size() >= n ? sim.event_ticks[size_t(p[n - 1])] + delay
                                   : start + uint32_t(n) * first_pass;
        };

        // The loop channel's passes before the marked loop, and up to the song's end.
        const size_t skip = start_pass == LoopStart::kSecondPass ? sim.times : 0;
        const size_t last = size_t(loops) * sim.times + skip;
        plan.loops = true;
        plan.loop_start = pass_end(skip);
        plan.loop_end = pass_end(skip + sim.times);
        plan.next_end = pass_end(skip + 2 * sim.times);
        plan.end = p.size() >= last ? std::min(pass_end(last), sim.frame_ticks.back()) : sim.frame_ticks.back();
    }
    else
    {
        for (const ChannelLoop& channel : sim.channel_loops)
        {
            if (channel.end >= 0)
            {
                plan.end = std::max(plan.end, sim.event_ticks[size_t(channel.end)]);
            }
        }
    }

    if (plan.end == 0)
    {
        plan.end = sim.frame_ticks.back();
    }
}

// Runs the model until every channel has looped, ended or stopped at its loop point and the song has played its loop
// `loops` times. Then works out the MIDI file's timing. From LoopStart::kSecondPass, the song plays the first pass
// through the loop before the marked loop. With `record`, the model records a performance too.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, Timing timing, LoopStart start_pass,
                    bool record = false)
{
    Simulation sim = RunModel(rom, info, song, loops + (start_pass == LoopStart::kSecondPass ? 1 : 0), record);
    PlanLoop(sim, timing, loops, start_pass);

    return sim;
}

} // namespace
} // namespace agblib::sequence_export::quintet
