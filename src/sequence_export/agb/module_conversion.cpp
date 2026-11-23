// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/agb/module_conversion.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/files.h"
#include "agblib/agb/song_plan.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/module_bars.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::agb
{

using agblib::agb::PassComparison;
using agblib::agb::WriteFile;

namespace
{

// Returns whether a channel sounds in the rows from `first` up to `last`.
bool Sounds(const ModuleRun& run, size_t first, size_t last)
{
    for (size_t i = run.rows[first].step; i < run.rows[last].step; i++)
    {
        if (run.steps[i].sounds)
        {
            return true;
        }
    }

    return false;
}

} // namespace

ModuleRecorder::ModuleRecorder(ModuleRun& run, int loops, LoopStart start_pass)
    : run_(run), loops_(loops), start_pass_(start_pass)
{
}

bool ModuleRecorder::Wants() const
{
    return run_.rows.empty() || run_.rows.size() <= end_;
}

bool ModuleRecorder::Full() const
{
    return !run_.rows.empty() && run_.rows.size() > end_;
}

bool ModuleRecorder::Ended() const
{
    return end_ != SIZE_MAX;
}

void ModuleRecorder::EndAt(size_t row)
{
    end_ = row;
}

void ModuleRecorder::Visit(const Place& place)
{
    if (Ended())
    {
        return;
    }

    // The first row that comes back to an earlier row's place starts the loop's second pass.
    const auto [it, added] = places_.emplace(place, run_.rows.size());
    if (!added)
    {
        const size_t length = run_.rows.size() - it->second;
        const size_t skip = start_pass_ == LoopStart::kSecondPass ? length : 0;
        run_.plan.loops = true;
        run_.plan.loop_start = it->second + skip;
        run_.plan.loop_end = run_.rows.size() + skip;
        end_ = run_.plan.loop_start + size_t(std::max(loops_, 1)) * length;
    }
}

void ModuleRecorder::AddRow(int order, int pattern_row, uint32_t samples)
{
    run_.rows.push_back({run_.steps.size(), order, pattern_row, samples});
}

void ModuleRecorder::AddStep(uint64_t time, bool sounds)
{
    run_.steps.push_back({time, run_.rows.size() - 1, sounds});
}

void ModuleRecorder::Finish(bool playing, const std::string& cut_off)
{
    if (run_.rows.empty())
    {
        return;
    }

    // A loop whose first pass doesn't end within the limit doesn't count. The passes after one that does end there.
    if (end_ != SIZE_MAX && run_.rows.size() <= end_)
    {
        if (run_.plan.loops && run_.plan.loop_end >= run_.rows.size())
        {
            run_.plan = {};
            end_ = SIZE_MAX;
        }
        else
        {
            end_ = run_.rows.size() - 1;
        }
    }

    // A module that the player can't play on ends where it stops, and one that doesn't loop within the limit is cut
    // off.
    if (end_ == SIZE_MAX)
    {
        end_ = run_.rows.size() - 1;
        if (playing)
        {
            run_.warnings.push_back(cut_off);
            run_.truncated = true;
        }
    }

    run_.rows.resize(std::min(run_.rows.size(), end_ + 1));
    if (run_.plan.loops && !Sounds(run_, run_.plan.loop_start, std::min(run_.plan.loop_end, run_.rows.size() - 1)))
    {
        run_.plan.loops = false;
        run_.rows.resize(run_.plan.loop_start + 1);
    }
    run_.plan.end = run_.rows.size() - 1;
    run_.steps.resize(run_.rows.back().step + 1);
}

ModuleTiming MakeModuleTiming(const ModuleRun& run)
{
    // The rows, and the row of the commonest length.
    ModuleTiming t;
    std::vector<ModuleStep> rows;
    std::map<uint32_t, size_t> lengths;
    for (size_t r = 0; r < run.rows.size(); r++)
    {
        t.rows.push_back(uint32_t(r) * kModuleRowTicks);
        rows.push_back({true, run.rows[r].order, run.rows[r].pattern_row, kModuleRowTicks});
        lengths[run.rows[r].samples]++;
    }
    t.rows.push_back(uint32_t(run.rows.size()) * kModuleRowTicks);

    const auto fewer = [](const auto& a, const auto& b)
    {
        return a.second < b.second;
    };
    const uint32_t usual = std::max_element(lengths.begin(), lengths.end(), fewer)->first;

    // The beat, from the commonest length of row, and the bars.
    const std::vector<OrderRun> runs = OrderRuns(rows);
    t.meter = ChooseMeter(t.rows, runs, kModuleRowTicks, double(usual) / run.rate / kModuleRowTicks);
    t.bars = BarStarts(rows, t.rows, runs, BarTicks(t.meter));

    // A tempo that sets the length of each row.
    uint32_t tempo = 0;
    for (size_t r = 0; r < run.rows.size(); r++)
    {
        const double seconds = double(run.rows[r].samples) / run.rate * t.meter.quarter_ticks / kModuleRowTicks;
        const uint32_t micros = uint32_t(std::lround(seconds * 1e6));
        if (micros != tempo)
        {
            t.tempos.push_back({t.rows[r], micros});
            tempo = micros;
        }
    }

    // Each step's MIDI tick: its row's, plus its share of the row.
    for (const ModuleRunStep& d : run.steps)
    {
        const ModuleRow& row = run.rows[d.row];
        const uint64_t within = d.time - run.steps[row.step].time;
        const uint64_t ticks = row.samples ? (within * kModuleRowTicks + row.samples / 2) / row.samples : 0;
        t.steps.push_back(t.rows[d.row] + uint32_t(std::min<uint64_t>(ticks, kModuleRowTicks)));
    }

    return t;
}

bool PassesAlike(const ModuleRun& run, const std::vector<std::vector<ModuleNote>>& notes)
{
    const ModuleTiming timing = MakeModuleTiming(run);
    const ModulePlan& plan = run.plan;
    PassComparison passes(timing.rows[plan.loop_start], timing.rows[plan.loop_end] - timing.rows[plan.loop_start]);
    for (size_t c = 0; c < notes.size(); c++)
    {
        for (const ModuleNote& n : notes[c])
        {
            const ModuleSound& s = n.sounds.front();
            const double row_samples = run.rows[run.steps[n.on].row].samples;
            passes.Add(timing.steps[n.on], int(c), n.key, {double(n.program), s.bend, s.left, s.right, row_samples});
        }
    }

    return passes.Alike();
}

void ReportModuleTimes(const ModuleRun& run, const ModuleTiming& timing, SongReport& report)
{
    const auto seconds = [&](size_t row)
    {
        return double(run.steps[run.rows[row].step].time) / run.rate;
    };
    report.seconds = seconds(run.plan.end);
    if (run.plan.loops)
    {
        report.loop_start = seconds(run.plan.loop_start);
        report.loop_end = seconds(run.plan.loop_end);
    }
    report.bpm = 60e6 / timing.tempos.front().second;
}

std::vector<uint8_t> ModuleMidiBytes(const std::string& title, const std::string& about, const ModuleRun& run,
                                     const ModuleTiming& timing, const std::vector<std::vector<ModuleNote>>& notes)
{
    const ModulePlan& plan = run.plan;
    const uint32_t end = timing.rows[plan.end];
    MidiFile midi(uint16_t(timing.meter.quarter_ticks));
    WriteModuleConductor(midi.AddTrack(), title, about, timing, timing.rows, plan);

    // The loop's start, from which each channel writes its settings again, unless the loop starts with the song.
    const uint32_t loop_start = plan.loops ? timing.rows[plan.loop_start] : 0;
    for (size_t c = 0; c < notes.size(); c++)
    {
        const std::vector<ModuleNote>& list = notes[c];
        if (list.empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        mt.Name("Channel " + std::to_string(c + 1));
        mt.SetEnd(end);

        // The pitch bend range covers the channel's largest bend.
        double widest = 0;
        for (const ModuleNote& n : list)
        {
            for (const ModuleSound& s : n.sounds)
            {
                widest = std::max(widest, std::fabs(s.bend));
            }
        }
        const int range = BendRangeFor(widest, kSmallBend);

        ChannelWriter writer(mt, MelodicChannel(int(c)), list[0].program, NumberedPreset(list[0].program), range,
                             loop_start);
        for (const ModuleNote& n : list)
        {
            const uint32_t on = timing.steps[n.on];
            const uint32_t off = timing.steps[n.off];
            if (on >= off)
            {
                continue;
            }

            // Each row's and tick's levels and bend.
            writer.SetProgram(on, n.program, NumberedPreset(n.program));
            for (const ModuleSound& s : n.sounds)
            {
                const uint32_t at = timing.steps[s.step];
                writer.SetLevels(at, s.left, s.right);
                writer.SetBend(at, s.bend);
            }
            writer.Note(on, off, n.key);
        }
    }

    return midi.Encode();
}

bool WriteModuleFiles(const std::string& stem, std::vector<uint8_t> midi, const Sf2File* soundfont, SongReport& report,
                      std::vector<uint8_t>* midi_bytes, std::vector<uint8_t>* sf2_bytes)
{
    std::string error;
    if (midi_bytes)
    {
        *midi_bytes = std::move(midi);
    }
    else
    {
        report.midi_path = stem + ".mid";
        if (!WriteFile(report.midi_path, midi, error))
        {
            report.warnings.push_back(error);
            return false;
        }
    }

    // The song's SoundFont, unless the instruments go in a shared one.
    if (soundfont)
    {
        if (!midi_bytes)
        {
            report.sf2_path = stem + ".sf2";
        }
        if (midi_bytes ? !soundfont->Encode(*sf2_bytes, error) : !soundfont->Write(report.sf2_path, error))
        {
            report.warnings.push_back(error);
            return false;
        }
    }

    return true;
}

} // namespace agblib::sequence_export::agb
