// SPDX-License-Identifier: MIT

// The simulation of a Krawall module, its notes, and the MIDI file's timing. Only convert.cpp includes this header. Its
// definitions therefore keep internal linkage.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <vector>

#include "convert_internal.h"

namespace agblib::sequence_export::krawall
{
namespace
{

// The most rows that a beat, or an eighth note in 12/8, can have.
constexpr uint32_t kMaxBeatRows = 64;

// The cents by which a note's pitch can miss its key before its bend makes up the difference: more than the table of
// periods misses equal temperament by in the octaves that modules use.
constexpr double kTuningCents = 3;

// The longest conversion, in seconds.
constexpr uint64_t kMaxSeconds = 60 * 60;

// Returns a channel's mixer channel after a tick. `scale` is the output's level for a mixer level of 1, where 128 is
// full scale.
Voice VoiceOf(const Player& player, const DriverInfo& info, const Channel& ch, double scale)
{
    Voice v;
    v.entry = ch.entry;
    v.playing = player.Sounding(ch);
    if (!v.playing)
    {
        return v;
    }

    const MixChannel& m = player.MixerOf(ch);
    v.handle = ch.handle;
    v.moves = m.moves;
    v.sample = m.sample;
    v.sample_index = m.sample_index;
    v.offset = m.pos - m.start;
    v.rate = std::fabs(double(m.inc)) * info.mix_rate / 65536.0;
    v.left = m.left * scale;
    v.right = (info.stereo ? m.right : m.left) * scale;

    return v;
}

// Runs the model until a row revisits an earlier module position with the same speed, tempo and pattern-loop state, and
// the module has played the loop between them `loops` times in all. From LoopStart::kSecondPass, the loop starts at the
// module's second pass through it. The module then plays the first pass before it. A song of a module that separates
// songs with +++ plays in song mode. Song mode loops back to the song's start at the song's end.
Simulation Simulate(const Rom& rom, const DriverInfo& info, const ModuleSong& song, int loops, LoopStart start_pass)
{
    Simulation sim;
    Player player(rom, info, song);
    sim.channels = player.Module().channels;
    sim.mix_rate = info.mix_rate;
    if (!player.Valid())
    {
        sim.warnings = player.Warnings();
        return sim;
    }

    const double scale = player.OutputScale();
    std::map<std::tuple<int, int, uint32_t, int, int>, size_t> rows;
    uint64_t first = 0;
    size_t end = SIZE_MAX;
    const auto record = [&](uint64_t time)
    {
        if (sim.ticks.size() > end)
        {
            return;
        }

        if (sim.ticks.empty())
        {
            first = time;
        }

        TickData d;
        d.time = time - first;
        d.length = player.TickSamples();
        d.row = player.TickInRow() == 0;
        d.order = player.RowOrder();
        d.pattern_row = player.RowNumber();
        d.speed = std::max(player.Speed(), 1);
        d.row_data = player.RowData();
        d.row_end = player.RowEnd();
        for (int c = 0; c < sim.channels; c++)
        {
            const Channel& channel = player.Channels()[size_t(c)];
            d.voices[size_t(c)] = VoiceOf(player, info, channel, scale);
            d.levels[size_t(c)] = {channel.volume, channel.panning};
        }

        // The first row that comes back to an earlier row's place starts the loop's second pass.
        if (d.row && end == SIZE_MAX)
        {
            const auto key = std::make_tuple(player.RowOrder(), player.RowNumber(), player.LoopState(), player.Speed(),
                                             player.Tempo());
            const auto [it, added] = rows.emplace(key, sim.ticks.size());
            if (!added)
            {
                const size_t length = sim.ticks.size() - it->second;
                const size_t skip = start_pass == LoopStart::kSecondPass ? length : 0;
                sim.plan.loops = true;
                sim.plan.loop_start = it->second + skip;
                sim.plan.loop_end = sim.ticks.size() + skip;
                end = sim.plan.loop_start + size_t(std::max(loops, 1)) * length;
            }
        }

        sim.ticks.push_back(d);
    };
    player.OnTick(record);

    const uint64_t most = kMaxSeconds * uint64_t(info.mix_rate);
    while (player.Playing() && sim.ticks.size() <= end && player.Elapsed() < most)
    {
        player.Frame();
    }

    if (sim.ticks.empty())
    {
        sim.warnings = player.Warnings();
        return sim;
    }

    if (end == SIZE_MAX)
    {
        // A module that stops ends where it stops, and one that neither stops nor loops is cut off.
        end = sim.ticks.size() - 1;
        if (player.Playing())
        {
            sim.warnings.push_back("found no loop within an hour, so the module is cut off there");
            sim.truncated = true;
        }
    }

    sim.ticks.resize(std::min(sim.ticks.size(), end + 1));
    sim.plan.end = sim.ticks.size() - 1;
    sim.warnings.insert(sim.warnings.end(), player.Warnings().begin(), player.Warnings().end());

    return sim;
}

// Returns the notes of channel `c`, adding their instruments to `instruments`.
std::vector<Note> MakeNotes(const Rom& rom, const DriverInfo& info, const Simulation& sim, int c,
                            InstrumentSet& instruments)
{
    std::vector<Note> notes;
    bool open = false;
    double start_rate = 0, tuning = 0;
    for (size_t i = 0; i <= sim.plan.end; i++)
    {
        const Voice& v = sim.ticks[i].voices[size_t(c)];
        const Voice* last = i > 0 ? &sim.ticks[i - 1].voices[size_t(c)] : nullptr;
        const bool started =
            v.playing && (!last || !last->playing || v.handle != last->handle || v.moves != last->moves);
        if (open && (!v.playing || started || i == sim.plan.end))
        {
            notes.back().off = i;
            open = false;
        }

        if (i == sim.plan.end)
        {
            break;
        }

        // A note's key is the one nearest its pitch, and a pitch that misses it by more than the table of periods does
        // starts with a bend. A sample that starts at no pitch starts its note when it has one.
        if (v.playing && !open && v.rate > 0)
        {
            Note n;
            n.on = i;
            n.entry = v.entry;
            n.program = instruments.Program(v.sample, v.sample_index, v.offset);
            const double semitones = 12 * std::log2(v.rate / SampleRate(rom, info, v.sample));
            n.key = std::clamp(int(std::lround(kRootKey + semitones)), 0, 127);
            tuning = semitones - (n.key - kRootKey);
            if (std::fabs(tuning) * 100 < kTuningCents)
            {
                tuning = 0;
            }

            start_rate = v.rate;
            notes.push_back(n);
            open = true;
        }

        // Each tick of the note: its pitch, as a bend from its key, and its levels.
        if (open)
        {
            Sound s;
            s.tick = i;
            s.bend = notes.back().sounds.empty() ? tuning : notes.back().sounds.back().bend;
            if (v.rate > 0)
            {
                s.bend = tuning + 12 * std::log2(v.rate / start_rate);
            }

            s.left = v.left;
            s.right = v.right;
            notes.back().sounds.push_back(s);
        }
    }

    return notes;
}

// Returns true if the notes of the module's second pass through its loop start as those of its first do: on the same
// channels and ticks from the pass's start, with the same sample, start point, key, bend and levels, and ticks of the
// same length. The module's channels keep their pan and volume when it comes back to its loop. The first pass can
// therefore play unlike the later ones. `sim` has to contain two passes of the loop.
bool FirstPassRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    const Plan& plan = sim.plan;
    PassComparison passes(plan.loop_start, plan.loop_end - plan.loop_start);
    InstrumentSet instruments;
    for (int c = 0; c < sim.channels; c++)
    {
        for (const Note& n : MakeNotes(rom, info, sim, c, instruments))
        {
            const Sound& s = n.sounds.front();
            passes.Add(n.on, c, n.key, {double(n.program), s.bend, s.left, s.right, double(sim.ticks[n.on].length)});
        }
    }

    return passes.Alike();
}

// Returns the key with the largest count, the first of those that tie.
template <typename K>
K Commonest(const std::map<K, size_t>& counts)
{
    K best{};
    size_t most = 0;
    for (const auto& [key, count] : counts)
    {
        if (count > most)
        {
            best = key;
            most = count;
        }
    }

    return best;
}

// Returns whether one speed is a power-of-two multiple of the other. These speeds share a beat grid: rows divide or
// combine evenly.
bool SameGrid(uint32_t speed, uint32_t main)
{
    const uint32_t high = std::max(speed, main), low = std::min(speed, main);
    return low > 0 && high % low == 0 && ((high / low) & (high / low - 1)) == 0;
}

Timing MakeTiming(const Simulation& sim)
{
    // The rows at each speed, and the ticks of each length.
    Timing t;
    std::map<int, size_t> speeds;
    std::map<uint32_t, size_t> lengths;
    for (const TickData& d : sim.ticks)
    {
        lengths[d.length]++;
        if (d.row)
        {
            speeds[d.speed]++;
        }
    }

    const uint32_t main = uint32_t(std::max(Commonest(speeds), 1));

    // Increase the MIDI resolution so rows at other speeds can be divided evenly into player ticks. The ticks per
    // quarter note must fit in 15 bits. If a speed would require a higher resolution, leave its player ticks on the
    // normal grid.
    uint32_t scale = 1;
    std::set<uint32_t> stretched;
    for (const auto& entry : speeds)
    {
        const uint32_t s = uint32_t(entry.first);
        const uint32_t need = std::lcm(scale, s / std::gcd(s, main));
        if (!SameGrid(s, main) && 2 * kMaxBeatRows * main * need <= 32767)
        {
            scale = need;
            stretched.insert(s);
        }
    }

    // Each tick's MIDI tick.
    uint32_t at = 0;
    for (const TickData& d : sim.ticks)
    {
        t.ticks.push_back(at);
        const uint32_t s = uint32_t(d.speed);
        at += stretched.count(s) ? main * scale / s : scale;
    }

    t.ticks.push_back(at);

    // The beat, from the commonest length of tick, and the bars. A row lasts its speed's count of ticks.
    std::vector<ModuleStep> steps;
    for (size_t i = 0; i < sim.ticks.size(); i++)
    {
        const TickData& d = sim.ticks[i];
        steps.push_back({d.row, d.order, d.pattern_row, (t.ticks[i + 1] - t.ticks[i]) * uint32_t(d.speed)});
    }

    const std::vector<OrderRun> runs = OrderRuns(steps);
    t.meter = ChooseMeter(t.ticks, runs, main * scale, double(Commonest(lengths)) / sim.mix_rate / scale);
    t.bars = BarStarts(steps, t.ticks, runs, BarTicks(t.meter));

    // A tempo that gives each tick its length.
    uint32_t tempo = 0;
    for (size_t i = 0; i < sim.ticks.size(); i++)
    {
        const uint32_t s = uint32_t(sim.ticks[i].speed);
        const double stretch = stretched.count(s) ? double(s) / main : 1;
        const double seconds = double(sim.ticks[i].length) / sim.mix_rate * t.meter.quarter_ticks / scale * stretch;
        const uint32_t micros = uint32_t(std::lround(seconds * 1e6));
        if (micros != tempo)
        {
            t.tempos.push_back({t.ticks[i], micros});
            tempo = micros;
        }
    }

    return t;
}

} // namespace
} // namespace agblib::sequence_export::krawall
