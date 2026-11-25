// SPDX-License-Identifier: MIT

// The stages that the conversions of the drivers whose models play modules share. The model plays the song, and the
// conversion reads the mixer's channels after each row and each tick of effects. These stages record that run and
// find its loop, make each channel's notes, time the rows in MIDI ticks, check whether the loop's first pass plays as
// the later ones do, and write the MIDI file. RunModuleConversion() runs them in order for a song.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/module_bars.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::agb
{

using agblib::agb::LoopStart;

// The MIDI ticks of a row. The ticks of the player's effects don't keep step with the rows. Each one therefore falls on
// the MIDI tick nearest its place in its row.
constexpr uint32_t kModuleRowTicks = 120;

// The cents by which a note's pitch can miss its key before its bend makes up the difference: more than the table of
// periods misses equal temperament by in the octaves that modules use.
constexpr double kModuleTuningCents = 3;

// The longest conversion, in seconds.
constexpr uint64_t kModuleMaxSeconds = 60 * 60;

// Returns the warning of a conversion of song `song` when the driver has no such song.
inline std::string MissingSongWarning(int song)
{
    return "there's no song " + std::to_string(song);
}

// Returns the report of a conversion of song `song` when the driver has no such song: not converted, with the warning.
inline SongReport MissingSongReport(int song)
{
    SongReport report;
    report.warnings.push_back(MissingSongWarning(song));
    return report;
}

// Returns the samples that a pass of a loop lasts where the mixer checks the loop's end once every `spacing` samples
// and restarts the loop with the place's fraction at 0: the first multiple of `spacing` whose samples at the step
// `step` cover the loop's `span`. `span` and `step` have the same fraction bits.
constexpr uint64_t LoopPassSamples(uint64_t span, uint64_t step, uint64_t spacing)
{
    const uint64_t stride = spacing * step;
    return spacing * ((span + stride - 1) / stride);
}

// Returns true if a loop whose passes play at `ratio` times the step's pitch plays kModuleTuningCents or more flat. The
// note then bends to the pitch of the passes.
inline bool LoopPassesBend(double ratio)
{
    return -1200 * std::log2(ratio) >= kModuleTuningCents;
}

// A step of the model's run: a row or a tick of effects.
struct ModuleRunStep
{
    uint64_t time = 0;   // the mixer's samples from the song's start
    size_t row = 0;      // the row that it belongs to: a place in the rows
    bool sounds = false; // a channel plays at a level above 0 after the step
};

// A row that the player played.
struct ModuleRow
{
    size_t step = 0; // its place in the steps
    int order = 0;
    int pattern_row = 0;  // its number in its pattern
    uint32_t samples = 0; // its length
};

// The stretch of a song that the conversion covers, in rows.
struct ModulePlan
{
    bool loops = false;
    size_t loop_start = 0; // the first row whose place in the module comes back
    size_t loop_end = 0;   // where it comes back
    size_t end = 0;
};

// The model's output for the stretch of a song that the plan covers, apart from the channels.
struct ModuleRun
{
    std::vector<ModuleRunStep> steps; // up to and including the plan's last row
    std::vector<ModuleRow> rows;
    ModulePlan plan;
    uint32_t channels = 0;
    double rate = 0;        // the mixer's rate in hertz
    bool truncated = false; // cut off without a loop
    std::vector<std::string> warnings;
};

// The run with each channel as the conversion reads it after each step. `Voice` is a driver's record of a channel. It
// has the fields `playing`, `starts` (the channel's count of notes and changes of sample), `sample` (the sample as the
// note plays it, with a `rate` in hertz at the SoundFont's root key), `number` (the sample's number in its module),
// `rate` (the rate in hertz at which the channel plays the sample's points), and `left` and `right` (the levels of the
// sides, where 128 is full scale).
template <typename Voice>
struct ModuleSimulation : ModuleRun
{
    // Returns channel `channel` after step `step`.
    const Voice& VoiceAt(size_t step, uint32_t channel) const
    {
        return voices[step * channels + channel];
    }

    std::vector<Voice> voices; // `channels` a step, step by step
};

// Returns true if a channel plays at a level above 0.
template <typename Voice>
bool Sounding(const Voice& v)
{
    return v.playing && (v.left > 0 || v.right > 0);
}

// Records a model's run of a module and finds its loop. The first row that comes back to an earlier row's place in the
// module starts the loop's second pass. The run then goes on until the module has played the loop `loops` times in
// all. From LoopStart::kSecondPass, the loop starts at the module's second pass through it. The module then plays the
// first pass before it.
class ModuleRecorder
{
public:
    // A row's place in the module: its order, its number in its pattern, and what else decides how the module plays on
    // from it, such as the speed and the tempo.
    using Place = std::tuple<int, int, std::vector<uint32_t>>;

    // Records into `run`. The run has to be empty.
    ModuleRecorder(ModuleRun& run, int loops, LoopStart start_pass);

    // Returns true while the run requires more rows.
    bool Wants() const;

    // Returns true once the run has every row up to its end. A step after that is left out.
    bool Full() const;

    // Returns true once the run's end is known.
    bool Ended() const;

    // Ends the run at row `row`, the last that it plays.
    void EndAt(size_t row);

    // Looks up the place of the next row, before AddRow() adds it, unless the run's end is known.
    void Visit(const Place& place);

    // Adds a row that starts at the next step.
    void AddRow(int order, int pattern_row, uint32_t samples);

    // Adds a step at `time`. `sounds` is set if a channel plays at a level above 0 after it.
    void AddStep(uint64_t time, bool sounds);

    // Ends the recording once the model has stopped, or played up to the end or the limit. `playing` is set if the
    // model could play on. `cut_off` is the warning for a song cut off at the limit without a loop. A loop whose
    // first pass doesn't end within the limit doesn't count. A loop in which no channel sounds is the wait after a
    // jingle: the song then has no loop and ends where that loop starts.
    void Finish(bool playing, const std::string& cut_off);

private:
    ModuleRun& run_;
    int loops_;
    LoopStart start_pass_;
    std::map<Place, size_t> places_;
    size_t end_ = SIZE_MAX;
};

// Trims the channels of `sim` to its steps, after ModuleRecorder::Finish().
template <typename Voice>
void TrimVoices(ModuleSimulation<Voice>& sim)
{
    sim.voices.resize(sim.steps.size() * sim.channels);
}

// A row or a tick of a note: how far its pitch is from the note's key, in semitones, and the levels of its sides.
struct ModuleSound
{
    size_t step = 0;
    double bend = 0;
    double left = 0;
    double right = 0;
};

// A note: a sample that a channel plays from the step that starts it until it stops, another starts or the song ends.
struct ModuleNote
{
    size_t on = 0; // in steps
    size_t off = 0;
    int key = 0;
    int program = 0;
    std::vector<ModuleSound> sounds;
};

// Returns the notes of channel `c`, adding their instruments to `instruments`. `Instruments` has the method
// `int Program(const Sample& sample, int number)`. A SoundFont sample plays its rate on key `root_key`.
template <typename Voice, typename Instruments>
std::vector<ModuleNote> MakeModuleNotes(const ModuleSimulation<Voice>& sim, uint32_t c, Instruments& instruments,
                                        int root_key)
{
    std::vector<ModuleNote> notes;
    bool open = false;
    double start_rate = 0, tuning = 0;
    const size_t end = sim.steps.size() - 1;
    for (size_t i = 0; i <= end; i++)
    {
        const Voice& v = sim.VoiceAt(i, c);
        const Voice* last = i > 0 ? &sim.VoiceAt(i - 1, c) : nullptr;
        const bool started = v.playing && (!last || !last->playing || v.starts != last->starts);
        if (open && (!v.playing || started || i == end))
        {
            notes.back().off = i;
            open = false;
        }

        if (i == end)
        {
            break;
        }

        // A note's key is the one nearest its pitch, and a pitch that misses it by more than the table of periods does
        // starts with a bend.
        if (v.playing && !open && v.rate > 0)
        {
            ModuleNote n;
            n.on = i;
            n.program = instruments.Program(v.sample, v.number);
            const double semitones = 12 * std::log2(v.rate / v.sample.rate);
            n.key = std::clamp(int(std::lround(root_key + semitones)), 0, 127);
            tuning = semitones - (n.key - root_key);
            if (std::fabs(tuning) * 100 < kModuleTuningCents)
            {
                tuning = 0;
            }

            start_rate = v.rate;
            notes.push_back(n);
            open = true;
        }

        // Each row and tick of the note: its pitch in a bend from its key, and its levels.
        if (open)
        {
            ModuleSound s;
            s.step = i;
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

// MIDI timing. Each row lasts kModuleRowTicks MIDI ticks, and the tempo sets its length.
struct ModuleTiming
{
    Meter meter;                                       // in MIDI ticks
    std::vector<uint32_t> rows;                        // the MIDI tick of each row, and of the end
    std::vector<uint32_t> steps;                       // the MIDI tick of each step
    std::vector<std::pair<uint32_t, uint32_t>> tempos; // MIDI tick and microseconds a quarter note
    std::vector<uint32_t> bars;                        // the MIDI ticks that start a bar
};

ModuleTiming MakeModuleTiming(const ModuleRun& run);

// Returns true if the notes of the song's second pass through its loop start as those of its first do: on the same
// channels and MIDI ticks from the pass's start, with the same sample, start point, key, bend and levels, in rows of
// the same length. A channel keeps its volume, its sample and its slide when the module comes back to its loop. The
// first pass can therefore play unlike the later ones. `run` has to contain two passes of the loop, and `notes` the
// notes of each of its channels.
bool PassesAlike(const ModuleRun& run, const std::vector<std::vector<ModuleNote>>& notes);

// Returns PassesAlike() for the notes of `sim`, whose programs come from a new set of instruments.
template <typename Instruments, typename Voice>
bool FirstPassRepeats(const ModuleSimulation<Voice>& sim, int root_key)
{
    Instruments instruments;
    std::vector<std::vector<ModuleNote>> notes;
    for (uint32_t c = 0; c < sim.channels; c++)
    {
        notes.push_back(MakeModuleNotes(sim, c, instruments, root_key));
    }

    return PassesAlike(sim, notes);
}

// Plays a song as a conversion does: the model's rows and ticks, and the notes of each channel that the track mask
// picks, with their instruments in `instruments`. `simulate(loops, start_pass)` returns the model's run. The marked
// loop starts at the song's second pass if its first plays unlike it. Returns false where the model played no row. The
// function writes the warnings and the count of tracks to the report.
template <typename Instruments, typename Voice, typename Simulate>
bool PlayModule(const Simulate& simulate, const ConvertSettings& settings, Instruments& instruments, int root_key,
                ModuleSimulation<Voice>& sim, std::vector<std::vector<ModuleNote>>& notes, SongReport& report)
{
    sim = simulate(settings.loops, LoopStart::kFirstPass);
    if (sim.plan.loops && !FirstPassRepeats<Instruments>(simulate(2, LoopStart::kFirstPass), root_key))
    {
        sim = simulate(settings.loops, LoopStart::kSecondPass);
    }

    report.warnings = sim.warnings;
    if (sim.rows.empty())
    {
        return false;
    }

    notes.assign(sim.channels, {});
    for (uint32_t c = 0; c < sim.channels; c++)
    {
        if ((settings.track_mask >> c) & 1)
        {
            notes[c] = MakeModuleNotes(sim, c, instruments, root_key);
            report.tracks += notes[c].empty() ? 0 : 1;
        }
    }

    return true;
}

// Sets the report's length, loop and tempo from the run's plan and the timing.
void ReportModuleTimes(const ModuleRun& run, const ModuleTiming& timing, SongReport& report);

// Writes the conductor track of a module's MIDI file: its name `title`, the description `about`, the bars' signatures
// and the tempo map of `timing`, and the loop's markers and the end of `plan`, at the MIDI ticks of `ticks`. `Timing`
// has the fields `meter`, `bars` and `tempos` of ModuleTiming, and `Plan` the fields of ModulePlan.
template <typename Timing, typename Plan>
void WriteModuleConductor(MidiTrack& conductor, const std::string& title, const std::string& about,
                          const Timing& timing, const std::vector<uint32_t>& ticks, const Plan& plan)
{
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    WriteBarSignatures(conductor, timing.bars, timing.meter);
    for (const auto& [tick, micros] : timing.tempos)
    {
        conductor.Tempo(tick, micros);
    }

    if (plan.loops)
    {
        conductor.Meta(ticks[plan.loop_start], 0x06, "loopStart");
        conductor.Meta(ticks[plan.loop_end], 0x06, "loopEnd");
    }

    conductor.SetEnd(ticks[plan.end]);
}

// The MIDI file's bytes: a conductor track with the tempo map, then a track for each channel that plays notes.
std::vector<uint8_t> ModuleMidiBytes(const std::string& title, const std::string& about, const ModuleRun& run,
                                     const ModuleTiming& timing, const std::vector<std::vector<ModuleNote>>& notes);

// The instruments of the conversions of modules: each is a sample as a note plays it, from a given point. `Played` is a
// driver's record of such a sample. It's ordered, and it has the point in its field `offset`. Instruments become
// SoundFont presets, numbered in order of first use.
template <typename Played>
class ModuleInstruments
{
public:
    // Returns the program of `sample`, number `number` in its module, adding it if it's new. Programs from 128 on go in
    // the banks after the first.
    int Program(const Played& sample, int number)
    {
        return programs_.try_emplace(sample, Instrument{number, int(programs_.size())}).first->second.program;
    }

    // Returns the SoundFont of the instruments. `make_sample(sample, name)` returns the SoundFont sample of `sample`
    // played from its first point. The instruments of every start point of a sample share it. A preset's name is
    // "Sample" and the sample's number, with "+" and the point for a note that starts within the sample.
    template <typename MakeSample>
    Sf2File Build(const MakeSample& make_sample) const
    {
        namespace sf2 = seqlib::sf2;
        Sf2File f;
        std::map<Played, int> samples;
        for (const auto& [played, inst] : programs_)
        {
            char name[32];
            std::snprintf(name, sizeof name, "Sample %02d", inst.number);

            // The SoundFont's sample. The instruments of every start point share it.
            Played whole = played;
            whole.offset = 0;
            auto it = samples.find(whole);
            if (it == samples.end())
            {
                f.samples.push_back(make_sample(whole, name));
                it = samples.emplace(whole, int(f.samples.size()) - 1).first;
            }

            // A note that starts within the sample starts its zone there.
            sf2::Zone z;
            z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, 0, 127));
            if (played.offset)
            {
                z.generators.push_back(sf2::Generator::Value(sf2::kStartAddrsOffset, int(played.offset % 32768)));
                if (played.offset >= 32768)
                {
                    z.generators.push_back(
                        sf2::Generator::Value(sf2::kStartAddrsCoarseOffset, int(played.offset / 32768)));
                }

                std::snprintf(name, sizeof name, "Sample %02d +%u", inst.number, unsigned(played.offset));
            }

            z.generators.push_back(
                sf2::Generator::Value(sf2::kSampleModes, Looped(f.samples[size_t(it->second)]) ? 1 : 0));
            z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, it->second));
            f.instruments.push_back(MakeInstrument(name, {z}));
            f.presets.push_back(MakePreset(name, uint16_t(inst.program / 128), uint16_t(inst.program % 128),
                                           int(f.instruments.size()) - 1));
        }

        return f;
    }

private:
    struct Instrument
    {
        int number = 0;
        int program = 0;
    };

    std::map<Played, Instrument> programs_;
};

// Writes a song's MIDI file to `stem`.mid, and `soundfont` to `stem`.sf2 unless it's null, and sets the report's
// paths. With `midi_bytes`, the files' bytes go to `midi_bytes` and `sf2_bytes` instead, and the report names no
// paths. Returns false after adding a warning to the report if a file can't be written.
bool WriteModuleFiles(const std::string& stem, std::vector<uint8_t> midi, const Sf2File* soundfont, SongReport& report,
                      std::vector<uint8_t>* midi_bytes, std::vector<uint8_t>* sf2_bytes);

// A song's files in memory, and the report of their conversion.
struct ModuleSongFiles
{
    SongReport report;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
};

// Converts song `song` of the game's `count` songs. It plays the song with PlayModule(), sets the report's times, and
// writes the files if `write` is set and the song isn't silent. `simulate(loops, start_pass)` returns the model's run,
// and a SoundFont sample plays its rate on key `root_key`. The MIDI file's description names the game, then `source`
// (where the song is in the image), then the program. If `shared` is supplied, the song's instruments go into it;
// otherwise a SoundFont of them is written next to the MIDI file. With `files`, the files' bytes go there instead of to
// files, and the report names no paths. `Instruments` has the methods `int Program(const Sample&, int number)` and
// `Sf2File Build(const Rom&) const`.
template <typename Instruments, typename Simulate>
SongReport RunModuleConversion(const agblib::agb::Rom& rom, int song, int count, const std::string& source,
                               int root_key, const Simulate& simulate, const ConvertSettings& settings,
                               Instruments* shared, bool write, ModuleSongFiles* files)
{
    SongReport report;
    Instruments song_instruments;
    Instruments& instruments = shared ? *shared : song_instruments;
    decltype(simulate(0, LoopStart::kFirstPass)) sim;
    std::vector<std::vector<ModuleNote>> notes;
    if (!PlayModule(simulate, settings, instruments, root_key, sim, notes, report))
    {
        return report;
    }

    const ModuleTiming timing = MakeModuleTiming(sim);
    ReportModuleTimes(sim, timing, report);
    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The files' names and titles, and the MIDI file's description.
    const std::string number = SongNumber(song, count);
    const std::string title = rom.Title() + " #" + number;
    char about[192];
    std::snprintf(about, sizeof about, "%s (%s) %s, converted by %s", rom.Title().c_str(), rom.GameCode().c_str(),
                  source.c_str(), settings.program.c_str());

    // The song's SoundFont, unless the instruments go in a shared one.
    Sf2File soundfont;
    if (!shared)
    {
        soundfont = song_instruments.Build(rom);
        soundfont.program = settings.program;
        soundfont.SetTitle(title);
    }

    if (!WriteModuleFiles(SongStem(settings, number), ModuleMidiBytes(title, about, sim, timing, notes),
                          shared ? nullptr : &soundfont, report, files ? &files->midi : nullptr,
                          files ? &files->sf2 : nullptr))
    {
        return report;
    }

    report.ok = true;

    return report;
}

// Ends a run of the model that `recorder` recorded into `sim`, once the model has stopped. Adds a warning where the
// player reached `unmodelled` paths that the model doesn't follow. Where a row played, finishes the recording
// (ModuleRecorder::Finish(), with `playing` set if the model could play on) and trims the channels. Each row then lasts
// until the next starts.
template <typename Voice>
void FinishModuleSimulation(ModuleSimulation<Voice>& sim, ModuleRecorder& recorder, bool playing, uint64_t unmodelled)
{
    if (unmodelled != 0)
    {
        sim.warnings.push_back("the player reached " + std::to_string(unmodelled) +
                               " paths that the model doesn't follow");
    }

    if (sim.rows.empty())
    {
        return;
    }

    recorder.Finish(playing, "found no loop within an hour, so the song is cut off there");
    TrimVoices(sim);

    for (size_t r = 0; r + 1 < sim.rows.size(); r++)
    {
        sim.rows[r].samples = uint32_t(sim.steps[sim.rows[r + 1].step].time - sim.steps[sim.rows[r].step].time);
    }
}

// Converts song `song` of `info.songs` with RunModuleConversion(), where each song plays a module of `info.modules`:
// its `module` is the module's place there, and the module has an `address`. `describe(song, module)` returns where the
// song is in the image. The report's address is the module's.
template <typename Instruments, typename Info, typename Describe, typename Simulate>
SongReport RunModuleSong(const agblib::agb::Rom& rom, const Info& info, int song, const Describe& describe,
                         int root_key, const Simulate& simulate, const ConvertSettings& settings, Instruments* shared,
                         bool write, ModuleSongFiles* files)
{
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        return MissingSongReport(song);
    }

    const auto& entry = info.songs[size_t(song)];
    const auto& module = info.modules[size_t(entry.module)];
    SongReport report = RunModuleConversion(rom, song, int(info.songs.size()), describe(entry, module), root_key,
                                            simulate, settings, shared, write, files);
    report.address = module.address;

    return report;
}

} // namespace agblib::sequence_export::agb
