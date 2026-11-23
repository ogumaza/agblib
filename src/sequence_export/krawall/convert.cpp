// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/krawall/convert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "agblib/sequence_export/agb/module_conversion.h"
#include "convert_internal.h"
#include "convert_simulation.h"
#include "converted_notes.h"

namespace agblib::sequence_export::krawall
{
namespace
{

// The MIDI channels other than the drum channel.
constexpr size_t kMelodicChannels = 15;

// The steps of CC10 or CC11, and the semitones of bend, by which two notes' settings can differ on one MIDI channel.
constexpr int kSettingTolerance = 3;
constexpr double kBendTolerance = 0.05;

// What a track sets for a note at a tick: its key, its pan and expression, and its bend. A key of -1 means no note.
struct TickState
{
    int key = -1;
    int pan = 64;
    int expression = 0;
    double bend = 0;
};

// Returns the settings that the channel's notes set at each tick up to the module's end.
std::vector<TickState> TickStates(const Simulation& sim, const std::vector<Note>& notes)
{
    std::vector<TickState> states(sim.plan.end);
    for (const Note& n : notes)
    {
        for (const Sound& s : n.sounds)
        {
            TickState& state = states[s.tick];
            state.key = n.key;
            LevelsToControllers(s.left, s.right, state.pan, state.expression);
            state.bend = s.bend;
        }
    }

    return states;
}

// Returns whether two notes at a tick require settings that one MIDI channel can't give both: another pan, expression
// or bend, or the same key, whose note off would end both.
bool Clashes(const TickState& a, const TickState& b)
{
    return a.key == b.key || std::abs(a.pan - b.pan) > kSettingTolerance ||
           std::abs(a.expression - b.expression) > kSettingTolerance || std::fabs(a.bend - b.bend) > kBendTolerance;
}

// Returns the mixer's samples in which two channels' notes sound at once with settings that clash.
uint64_t Clash(const Simulation& sim, const std::vector<TickState>& a, const std::vector<TickState>& b)
{
    uint64_t total = 0;
    for (size_t i = 0; i < a.size(); i++)
    {
        if (a[i].key >= 0 && b[i].key >= 0 && Clashes(a[i], b[i]))
        {
            total += sim.ticks[i + 1].time - sim.ticks[i].time;
        }
    }

    return total;
}

// Returns the notes without the silence at their ends: the ticks from which the mixer contains both of a note's levels
// at
// 0. The game's output has nothing of the note then. On a shared MIDI channel, another track's expression would make
// the note sound again. A note whose levels stay 0 throughout goes.
std::vector<Note> WithoutFinalSilence(const std::vector<Note>& notes)
{
    std::vector<Note> kept;
    for (Note n : notes)
    {
        size_t end = n.sounds.size();
        while (end > 0 && n.sounds[end - 1].left == 0 && n.sounds[end - 1].right == 0)
        {
            end--;
        }
        if (end == 0)
        {
            continue;
        }

        if (end < n.sounds.size())
        {
            n.off = n.sounds[end].tick;
            n.sounds.resize(end);
        }

        kept.push_back(std::move(n));
    }

    return kept;
}

// Returns each channel's MIDI channel, from the channels other than the drum channel. With up to 15 channels that play
// notes, each takes one. With more, channels share them. A MIDI channel's controllers and bend act on all its notes.
// Each channel starts in a group of its own. The two groups that clash least then merge until 15 are left. A tie goes
// to the lower channels. The groups take MIDI channels in the order of their lowest channels. `notes` contains the
// notes without their final silence.
std::vector<int> SharedMidiChannels(const Simulation& sim, const std::vector<std::vector<Note>>& notes)
{
    std::vector<size_t> playing;
    std::vector<std::vector<TickState>> states;
    for (size_t c = 0; c < notes.size(); c++)
    {
        if (!notes[c].empty())
        {
            playing.push_back(c);
            states.push_back(TickStates(sim, notes[c]));
        }
    }

    const size_t count = playing.size();
    std::vector<std::vector<uint64_t>> clash(count, std::vector<uint64_t>(count));
    for (size_t a = 0; a < count; a++)
    {
        for (size_t b = a + 1; b < count; b++)
        {
            clash[a][b] = clash[b][a] = Clash(sim, states[a], states[b]);
        }
    }

    std::vector<std::vector<size_t>> groups(count);
    for (size_t a = 0; a < count; a++)
    {
        groups[a] = {a};
    }
    while (groups.size() > kMelodicChannels)
    {
        size_t best_a = 0;
        size_t best_b = 1;
        uint64_t least = UINT64_MAX;
        for (size_t a = 0; a < groups.size(); a++)
        {
            for (size_t b = a + 1; b < groups.size(); b++)
            {
                uint64_t added = 0;
                for (size_t x : groups[a])
                {
                    for (size_t y : groups[b])
                    {
                        added += clash[x][y];
                    }
                }
                if (added < least)
                {
                    best_a = a;
                    best_b = b;
                    least = added;
                }
            }
        }

        groups[best_a].insert(groups[best_a].end(), groups[best_b].begin(), groups[best_b].end());
        groups.erase(groups.begin() + std::ptrdiff_t(best_b));
    }

    std::vector<int> midi(notes.size(), MelodicChannel(0));
    for (size_t g = 0; g < groups.size(); g++)
    {
        for (size_t x : groups[g])
        {
            midi[playing[x]] = MelodicChannel(int(g));
        }
    }

    return midi;
}

// Returns each channel's MIDI channel in a module where every channel that plays notes gets one. In a module with up to
// 15 channels, channel c takes the c-th. In a module with more, the 15 channels whose notes last longest take one each,
// in the channels' order.
std::vector<int> SeparateMidiChannels(const Simulation& sim, const std::vector<std::vector<Note>>& notes)
{
    std::vector<int> midi(notes.size());
    if (notes.size() <= kMelodicChannels)
    {
        for (size_t c = 0; c < notes.size(); c++)
        {
            midi[c] = MelodicChannel(int(c));
        }

        return midi;
    }

    std::vector<uint64_t> length(notes.size());
    for (size_t c = 0; c < notes.size(); c++)
    {
        for (const Note& n : notes[c])
        {
            length[c] += sim.ticks[n.off].time - sim.ticks[n.on].time;
        }
    }

    std::vector<size_t> order(notes.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return length[a] > length[b]; });

    std::vector<size_t> first(order.begin(), order.begin() + kMelodicChannels);
    std::sort(first.begin(), first.end());
    for (size_t k = 0; k < first.size(); k++)
    {
        midi[first[k]] = MelodicChannel(int(k));
    }

    return midi;
}

// The MIDI file's bytes: a conductor track with the tempo map, then a track for each channel that plays notes, on the
// MIDI channel that `channels` gives it.
std::vector<uint8_t> MidiBytes(const std::string& title, const std::string& about, const Simulation& sim,
                               const Timing& timing, const std::vector<std::vector<Note>>& notes,
                               const std::vector<int>& channels)
{
    const Plan& plan = sim.plan;
    const uint32_t end = timing.ticks[plan.end];
    MidiFile midi(uint16_t(timing.meter.quarter_ticks));
    agb::WriteModuleConductor(midi.AddTrack(), title, about, timing, timing.ticks, plan);

    // The loop's start, from which each channel writes its settings again, unless the loop starts with the module.
    const uint32_t loop_start = plan.loops ? timing.ticks[plan.loop_start] : 0;

    for (size_t c = 0; c < notes.size(); c++)
    {
        const std::vector<Note>& list = notes[c];
        if (list.empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        const int ch = channels[c];
        mt.Name("Channel " + std::to_string(c + 1));
        mt.SetEnd(end);

        // The pitch bend range covers the largest bend of the channels on the MIDI channel. The ticks at which another
        // channel's note ends are those from which this channel's note sets its settings again.
        double widest = 0;
        bool shared = false;
        std::vector<char> other_ends(plan.end + 1, 0);
        for (size_t other = 0; other < notes.size(); other++)
        {
            if (channels[other] != ch || notes[other].empty())
            {
                continue;
            }

            shared = shared || other != c;
            for (const Note& n : notes[other])
            {
                other_ends[n.off] = other_ends[n.off] || other != c;
                for (const Sound& s : n.sounds)
                {
                    widest = std::max(widest, std::fabs(s.bend));
                }
            }
        }

        const int range = BendRangeFor(widest, kSmallBend);

        ChannelWriter writer(mt, ch, list[0].program, NumberedPreset(list[0].program), range, loop_start, shared);
        for (const Note& n : list)
        {
            const uint32_t on = timing.ticks[n.on];
            const uint32_t off = timing.ticks[n.off];
            if (on >= off)
            {
                continue;
            }

            writer.SetProgram(on, n.program, NumberedPreset(n.program));

            // Each tick's levels and bend.
            for (const Sound& s : n.sounds)
            {
                const uint32_t at = timing.ticks[s.tick];
                if (other_ends[s.tick])
                {
                    writer.SetSettingsAgain();
                }

                writer.SetLevels(at, s.left, s.right);
                writer.SetBend(at, s.bend);
            }

            writer.Note(on, off, n.key);
        }
    }

    return midi.Encode();
}

// Plays a module as the conversion does: the model's ticks, the notes of each channel that the track mask picks, with
// their instruments in `instruments`, and each channel's MIDI channel. Returns false where the model played no tick.
// The report takes the warnings and the count of tracks.
bool PlayModule(const Rom& rom, const DriverInfo& info, const ModuleSong& entry, const ConvertSettings& settings,
                InstrumentSet& instruments, Simulation& sim, std::vector<std::vector<Note>>& notes,
                std::vector<int>& midi, SongReport& report)
{
    // The marked loop starts at the module's second pass if its first plays unlike it as when the channels come back
    // to the loop with the pan or volume that its end left them.
    sim = Simulate(rom, info, entry, settings.loops, LoopStart::kFirstPass);
    if (sim.plan.loops && !FirstPassRepeats(rom, info, Simulate(rom, info, entry, 2, LoopStart::kFirstPass)))
    {
        sim = Simulate(rom, info, entry, settings.loops, LoopStart::kSecondPass);
    }

    report.warnings = sim.warnings;
    if (sim.ticks.empty())
    {
        return false;
    }

    // The notes of each channel chosen.
    notes.assign(size_t(sim.channels), {});
    for (int c = 0; c < sim.channels; c++)
    {
        if ((settings.track_mask >> c) & 1)
        {
            notes[size_t(c)] = MakeNotes(rom, info, sim, c, instruments);
            report.tracks += notes[size_t(c)].empty() ? 0 : 1;
        }
    }

    // With more than 15 channels that play notes, channels share MIDI channels.
    const auto playing =
        size_t(std::count_if(notes.begin(), notes.end(), [](const auto& list) { return !list.empty(); }));
    if (playing <= kMelodicChannels)
    {
        midi = SeparateMidiChannels(sim, notes);
    }
    else
    {
        report.tracks = 0;
        for (std::vector<Note>& list : notes)
        {
            list = WithoutFinalSilence(list);
            report.tracks += list.empty() ? 0 : 1;
        }

        midi = SharedMidiChannels(sim, notes);
        std::vector<std::vector<TickState>> states;
        for (const std::vector<Note>& list : notes)
        {
            states.push_back(TickStates(sim, list));
        }

        for (size_t a = 0; a < notes.size(); a++)
        {
            for (size_t b = a + 1; b < notes.size(); b++)
            {
                const uint64_t clash = midi[a] == midi[b] ? Clash(sim, states[a], states[b]) : 0;
                if (clash > 0)
                {
                    char text[128];
                    std::snprintf(text, sizeof text,
                                  "channels %zu and %zu share a MIDI channel, and their notes clash for %.1f s", a + 1,
                                  b + 1, double(clash) / sim.mix_rate);
                    report.warnings.push_back(text);
                }
            }
        }
    }

    return true;
}

// Converts a module, writing its files if `write` is set. With `files`, the files' bytes go there instead of to files,
// and the report names no paths.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write, SongFiles* files = nullptr)
{
    SongReport report;
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        report.warnings.push_back("there's no module " + std::to_string(song));
        return report;
    }

    const ModuleSong& entry = info.songs[size_t(song)];
    const uint32_t module = entry.module;
    InstrumentSet own;
    InstrumentSet& instruments = shared ? *shared : own;
    Simulation sim;
    std::vector<std::vector<Note>> notes;
    std::vector<int> midi;
    if (!PlayModule(rom, info, entry, settings, instruments, sim, notes, midi, report))
    {
        return report;
    }

    const Timing timing = MakeTiming(sim);
    const double rate = sim.mix_rate;
    report.seconds = double(sim.ticks[sim.plan.end].time) / rate;
    if (sim.plan.loops)
    {
        report.loop_start = double(sim.ticks[sim.plan.loop_start].time) / rate;
        report.loop_end = double(sim.ticks[sim.plan.loop_end].time) / rate;
    }

    report.bpm = 60e6 / timing.tempos.front().second;

    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The files' names and titles, and the MIDI file's description. The description names the module by its place in
    // the list.
    const std::string number = SongNumber(song, int(info.songs.size()));
    const std::string title = rom.Title() + " #" + number;
    const int place = int(std::find(info.modules.begin(), info.modules.end(), module) - info.modules.begin());
    char about[192];
    if (entry.song < 0)
    {
        std::snprintf(about, sizeof about, "%s (%s) module %d at 0x%08X, converted by %s", rom.Title().c_str(),
                      rom.GameCode().c_str(), place, unsigned(module), settings.program.c_str());
    }
    else
    {
        std::snprintf(about, sizeof about, "%s (%s) song %d of module %d at 0x%08X, converted by %s",
                      rom.Title().c_str(), rom.GameCode().c_str(), entry.song, place, unsigned(module),
                      settings.program.c_str());
    }

    const std::string stem = SongStem(settings, number);

    // The module's SoundFont, unless the instruments go in a shared one.
    Sf2File soundfont;
    if (!shared)
    {
        soundfont = own.Build(rom, info);
        soundfont.program = settings.program;
        soundfont.SetTitle(title);
    }

    report.ok =
        agb::WriteModuleFiles(stem, MidiBytes(title, about, sim, timing, notes, midi), shared ? nullptr : &soundfont,
                              report, files ? &files->midi : nullptr, files ? &files->sf2 : nullptr);

    return report;
}

} // namespace

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    SongFiles files;
    files.report = Run(rom, info, song, settings, nullptr, true, &files);
    return files;
}

ConvertedModule ConvertedNotes(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    ConvertedModule out;
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        out.warnings.push_back("there's no module " + std::to_string(song));
        return out;
    }

    SongReport report;
    InstrumentSet instruments;
    Simulation sim;
    std::vector<std::vector<Note>> notes;
    std::vector<int> midi;
    const bool played =
        PlayModule(rom, info, info.songs[size_t(song)], settings, instruments, sim, notes, midi, report);
    out.warnings = report.warnings;
    if (!played)
    {
        return out;
    }

    const Timing timing = MakeTiming(sim);
    const Plan& plan = sim.plan;
    out.valid = true;
    out.truncated = sim.truncated;
    out.ticks_per_quarter = uint32_t(timing.meter.quarter_ticks);
    out.end = timing.ticks[plan.end];
    out.seconds = double(sim.ticks[plan.end].time) / sim.mix_rate;
    if (plan.loops)
    {
        out.loops = true;
        out.loop_start = timing.ticks[plan.loop_start];
        out.loop_end = timing.ticks[plan.loop_end];
    }

    for (const auto& [tick, micros] : timing.tempos)
    {
        out.tempos.emplace_back(tick, micros);
    }

    // The rows and the channels' levels up to the end.
    for (size_t i = 0; i < plan.end; i++)
    {
        const TickData& d = sim.ticks[i];
        if (d.row)
        {
            out.rows.push_back({timing.ticks[i], d.row_data, d.row_end, d.order, d.pattern_row});
        }

        for (int c = 0; c < sim.channels; c++)
        {
            const auto [volume, pan] = d.levels[size_t(c)];
            if (i == 0 || d.levels[size_t(c)] != sim.ticks[i - 1].levels[size_t(c)])
            {
                out.levels.push_back({c, timing.ticks[i], volume, pan});
            }
        }
    }

    // The notes that the MIDI file writes.
    for (size_t c = 0; c < notes.size(); c++)
    {
        for (const Note& n : notes[c])
        {
            const uint32_t on = timing.ticks[n.on];
            const uint32_t off = timing.ticks[n.off];
            if (on < off)
            {
                out.notes.push_back({int(c), n.entry, on, off, n.key, n.program});
            }
        }
    }

    return out;
}

} // namespace agblib::sequence_export::krawall
