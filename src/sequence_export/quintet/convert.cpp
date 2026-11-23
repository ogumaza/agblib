// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/quintet/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "agblib/sequence_export/agb/module_conversion.h"
#include "convert_internal.h"
#include "convert_notes.h"
#include "convert_simulation.h"
#include "converted_notes.h"

namespace agblib::sequence_export::quintet
{
namespace
{

// The start of a note in a pass through a loop. FirstPassRepeats() compares the passes by these fields: the MIDI ticks
// from the pass's start, the key and program, the controllers for the levels and the bend of the note's first frame,
// and the MIDI file's tempo there. A noise note's key is 0: its drum is compared on its own.
struct NoteStart
{
    bool operator==(const NoteStart& other) const = default;

    uint32_t at = 0;
    int key = 0;
    int program = 0;
    int cc10 = 0;
    int cc11 = 0;
    double bend = 0;
    int tempo = 0;
};

// A noise note's drum and the frames that the note spans.
struct NoiseNote
{
    NoiseSound drum;
    size_t frames = 0;
};

// Returns the frames of `drum` up to its `frames`th, without the frames at the end that change nothing.
NoiseSound NoiseUpTo(const NoiseSound& drum, size_t frames)
{
    NoiseSound sound = drum;
    sound.frames.resize(std::min(sound.frames.size(), frames));
    sound.Trim();

    return sound;
}

// Returns the bend of each of a note's frames, or nothing for a note that the hardware's sweep bends. The sweep's
// clock isn't tied to the frames. Its steps fall at other places in the frames from pass to pass.
std::vector<double> FrameBends(const Note& note)
{
    std::vector<double> bends;
    bends.reserve(note.sounds.size());
    for (const Sound& s : note.sounds)
    {
        if (!s.sweeps.empty())
        {
            return {};
        }

        bends.push_back(s.bend);
    }

    return bends;
}

// The levels of a frame of a note as the driver writes them (see Sound::envelope).
struct FrameLevel
{
    bool operator==(const FrameLevel& other) const = default;

    bool square = false;
    int envelope = -1;
    int sweep = -1;
    double left = 0, right = 0; // the scales of a square's sides, or the levels of another channel
};

// Returns the levels that the driver writes for each frame of a note.
std::vector<FrameLevel> FrameLevels(const Note& note)
{
    std::vector<FrameLevel> levels;
    levels.reserve(note.sounds.size());
    for (const Sound& s : note.sounds)
    {
        const bool square = s.envelope >= 0;
        levels.push_back({square, s.envelope, s.sweep, square ? s.side_left : s.left, square ? s.side_right : s.right});
    }

    return levels;
}

// Returns true if `a` and `b` are the same up to the shorter one's end.
template <typename T>
bool SameStart(const std::vector<T>& a, const std::vector<T>& b)
{
    const size_t frames = std::min(a.size(), b.size());
    return std::equal(a.begin(), a.begin() + std::ptrdiff_t(frames), b.begin());
}

// Returns true if each channel's second pass through its loop plays the same notes as its first: the same keys and
// programs at the same ticks from the pass's start, with the same levels, bend and tempo. Every note has the same
// velocity. The notes' ends aren't compared. The first pass can be a fraction of a frame longer than the next: the
// driver drops the part of a frame it has counted when it loops. The sound hardware's clocks aren't tied to the frames,
// and can end a note a frame earlier or later from pass to pass. The bends and the levels of the later frames of a note
// are compared up to the last frame that the notes of both passes span. That covers a slide, a vibrato and a volume
// envelope. The levels are those that the driver writes, without the hardware's envelope. On square 1, they include the
// sweep register (NR10). The bends of a note that the hardware's sweep bends aren't compared. A slide that runs at
// another speed in the first pass, or an envelope that only the later passes play, then moves the loop to the second
// pass. A noise note's drum contains every frame of the note. Its frames are compared up to the frames that both
// passes' notes span, and the spans may differ by a frame at most. `sim` has to contain three passes of the song's
// loop. Every channel's second pass is then complete.
bool FirstPassRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    NoteMaker maker(rom, info, sim);
    for (int c = 0; c < kChannels; c++)
    {
        // Where the channel's first pass starts, at its loop point, and where it and the second pass end.
        std::vector<uint32_t> bounds;
        for (size_t i = 0; i < sim.events.size() && bounds.size() < 3; i++)
        {
            const Event& e = sim.events[i];
            if (e.channel == c && e.kind == Event::kLoopPoint && bounds.size() < 2)
            {
                bounds.assign(1, sim.event_ticks[i]);
            }
            else if (e.channel == c && e.kind == Event::kLoop && !bounds.empty())
            {
                bounds.push_back(sim.event_ticks[i]);
            }
        }

        if (bounds.size() < 3)
        {
            continue;
        }

        std::array<std::vector<NoteStart>, 2> passes;
        std::array<std::vector<NoiseNote>, 2> noise;
        std::array<std::vector<std::vector<double>>, 2> bends;
        std::array<std::vector<std::vector<FrameLevel>>, 2> levels;
        for (const Note& n : maker.Make(c))
        {
            for (size_t k = 0; k < 2; k++)
            {
                if (n.on >= bounds[k] && n.on < bounds[k + 1])
                {
                    const auto frame = std::upper_bound(sim.frame_ticks.begin(), sim.frame_ticks.end(), n.on) - 1;
                    int cc10 = 64, cc11 = 0;
                    LevelsToControllers(n.sounds[0].left, n.sounds[0].right, cc10, cc11);
                    passes[k].push_back({n.on - bounds[k], c == kNoise ? 0 : n.key, n.program, cc10, cc11,
                                         n.sounds[0].bend,
                                         MidiTempo(sim.frames[size_t(frame - sim.frame_ticks.begin())])});
                    levels[k].push_back(FrameLevels(n));
                    if (c == kNoise)
                    {
                        noise[k].push_back({n.noise, n.sounds.size()});
                    }
                    else
                    {
                        bends[k].push_back(FrameBends(n));
                    }
                }
            }
        }

        if (passes[0] != passes[1])
        {
            return false;
        }

        for (size_t i = 0; i < levels[0].size(); i++)
        {
            if (!SameStart(levels[0][i], levels[1][i]))
            {
                return false;
            }
        }

        for (size_t i = 0; i < bends[0].size(); i++)
        {
            if (!SameStart(bends[0][i], bends[1][i]))
            {
                return false;
            }
        }

        for (size_t i = 0; i < noise[0].size(); i++)
        {
            const NoiseNote& a = noise[0][i];
            const NoiseNote& b = noise[1][i];
            const size_t frames = std::min(a.frames, b.frames);
            if (std::max(a.frames, b.frames) > frames + 1 ||
                NoiseUpTo(a.drum, frames).frames != NoiseUpTo(b.drum, frames).frames)
            {
                return false;
            }
        }
    }

    return true;
}

// Returns true if each channel starts the same number of notes in the marked loop and the next pass. When a channel
// loops, the driver discards its fractional frame. That fraction differs between channels. A channel's later passes can
// therefore start up to a frame earlier relative to the loop channel than its first did. The copy of a note at the
// loop's start can then fall before the end marker, causing a looping player to play it twice.
bool SeamRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    NoteMaker maker(rom, info, sim);
    const Plan& plan = sim.plan;
    for (int c = 0; c < kChannels; c++)
    {
        const std::vector<Note> notes = maker.Make(c);
        const auto starts = [&](uint32_t from, uint32_t to)
        {
            return std::count_if(notes.begin(), notes.end(), [&](const Note& n) { return n.on >= from && n.on < to; });
        };

        if (starts(plan.loop_start, plan.loop_end) != starts(plan.loop_end, plan.next_end))
        {
            return false;
        }
    }

    return true;
}

// Returns the MIDI tick of event `i`, the start of a stretch or a change of a channel's volume. The driver reads a
// channel's commands between the ends of two waits in one frame. They run at the tick of the wait that ends them. A
// tempo or a loop among them starts the channel's count again. The first note after it then starts its frame. So do
// the commands before the note.
uint32_t RunTick(const Simulation& sim, size_t i)
{
    const Event& at = sim.events[i];
    for (size_t j = i + 1; j < sim.events.size() && sim.events[j].frame == at.frame; j++)
    {
        const Event& e = sim.events[j];
        if (e.channel == at.channel && (e.kind == Event::kTempo || e.kind == Event::kLoop))
        {
            return sim.frame_ticks[at.frame];
        }
    }

    return sim.event_ticks[i];
}

// Returns the MIDI file's tempo changes before the end, as (MIDI tick, tempo).
std::vector<std::pair<uint32_t, int>> TempoMap(const Simulation& sim)
{
    std::vector<std::pair<uint32_t, int>> tempos;
    for (size_t f = 0; f < sim.frames.size() && (f == 0 || sim.frame_ticks[f] < sim.plan.end); f++)
    {
        const int tempo = MidiTempo(sim.frames[f]);
        if (tempos.empty() || tempos.back().second != tempo)
        {
            tempos.emplace_back(sim.frame_ticks[f], tempo);
        }
    }

    if (tempos.empty())
    {
        tempos.emplace_back(0, kDefaultTempo);
    }

    return tempos;
}

// Returns the tuning of the square zones, or with `shift`, of the zones of a wave whose notes play `shift` keys above
// a square's (see WaveKeyShift()). A square note on key kSquareBaseKey + i plays entry i of the frequency table. A
// zone plays the key in equal temperament. The keys outside the table take the tuning of its nearest entry.
KeyTuning PsgTuning(const Rom& rom, const DriverInfo& info, int shift = 0)
{
    KeyTuning tuning;
    for (int key = 0; key < int(tuning.size()); key++)
    {
        const int index = std::clamp(key - shift - kSquareBaseKey, 0, kFrequencyEntries - 1);
        const int setting = rom.U16(info.frequency_table + uint32_t(index) * 2) & 0x7FF;
        const double equal = KeyHz(kSquareBaseKey + index);
        tuning[size_t(key)] = SettingCents(setting, equal);
    }

    return tuning;
}

// Adds the SoundFont presets of a song's instruments, each in the bank and program of `slots` that its program number
// picks. The square and wave zones follow the frequency table (see PsgTuning()). Returns the warnings for instruments
// with nothing to play.
std::vector<std::string> AddPresets(const Rom& rom, const DriverInfo& info, SoundfontBuilder& sf,
                                    const NoteMaker& maker, const std::vector<BankProgram>& slots)
{
    const KeyTuning tuning = PsgTuning(rom, info);
    std::vector<std::string> warnings;
    for (const auto& [inst, program] : maker.Programs())
    {
        NamedZones made;
        switch (inst.kind)
        {
        case Instrument::kSquare:
            made = sf.SquareInstrument(inst.first, inst.frames, inst.second, tuning);
            break;

        case Instrument::kWave:
            made = sf.WaveInstrument(maker.Shapes(), inst.first, inst.frames, inst.second,
                                     PsgTuning(rom, info, WaveKeyShift(maker.Shapes()[inst.first])));
            break;

        case Instrument::kNoiseKit:
            for (const auto& [sound, key] : maker.Drums())
            {
                const int sample = sf.NoiseSample(sound);
                made.zones.push_back(RootKeyZone(sample, Looped(sf.File().samples[size_t(sample)]), key, key, key));
            }

            made.name = "Noise drums";
            break;

        case Instrument::kPcm:
            {
                const int sample = sf.PcmSample(inst.pcm);
                if (sample >= 0)
                {
                    made.zones.push_back(SampleZone(sample, inst.pcm.looped));
                }
                else
                {
                    warnings.push_back("a PCM note's sample has nothing to play");
                }

                made.name = AddressName("PCM", inst.pcm.start);
                break;
            }
        }

        const BankProgram& slot = slots[size_t(program)];
        sf.AddPreset(made.name, slot.bank, slot.program, sf.AddInstrument(made.name, std::move(made.zones)));
    }

    return warnings;
}

// Writes the MIDI file: a conductor track, then a track for each channel that plays notes. `slots` gives each program
// number its preset's bank and program.
// Returns the MIDI file's bytes.
std::vector<uint8_t> MidiBytes(const std::string& title, const std::string& about, const Simulation& sim,
                               const std::array<std::vector<Note>, kChannels>& notes,
                               const std::vector<BankProgram>& slots)
{
    const Plan& plan = sim.plan;
    const uint32_t end = plan.end;
    MidiFile midi(kTicksPerQuarter);
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    conductor.TimeSignature(0, 4, 2);
    for (const auto& [tick, tempo] : TempoMap(sim))
    {
        conductor.Tempo(tick, QuarterMicros(kTicksPerQuarter, uint64_t(tempo)));
    }

    if (plan.loops)
    {
        conductor.Meta(plan.loop_start, 0x06, "loopStart");
        conductor.Meta(plan.loop_end, 0x06, "loopEnd");
    }

    conductor.SetEnd(end);

    for (int c = 0; c < kChannels; c++)
    {
        const std::vector<Note>& list = notes[size_t(c)];
        if (list.empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        mt.Name(kChannelNames[c]);
        mt.SetEnd(end);

        // The pitch bend range covers the channel's largest bend. The other drivers that use BendRangeFor() leave out
        // bends of kSmallBend or less. Quintet's channels keep every bend, however small. A PCM note's rate, a whole
        // number in the driver, can bend the note by a few thousandths of a semitone from its key's rate.
        double widest = 0;
        for (const Note& n : list)
        {
            for (const Sound& s : n.sounds)
            {
                widest = std::max(widest, std::fabs(s.bend));
                for (const SweepStep& step : s.sweeps)
                {
                    widest = std::max(widest, std::fabs(step.bend));
                }
            }
        }

        const int range = BendRangeFor(widest, 0);

        const int first = list[0].program;
        ChannelWriter writer(mt, c, first, slots[size_t(first)], range, plan.loops ? plan.loop_start : 0);
        for (const Note& n : list)
        {
            writer.SetProgram(n.on, n.program, slots[size_t(n.program)]);

            // Each frame's levels and bend.
            for (const Sound& s : n.sounds)
            {
                const uint32_t tick = s.tick;
                if (tick >= n.off)
                {
                    break;
                }

                writer.SetLevels(tick, s.left, s.right);
                if (!range)
                {
                    continue;
                }

                // The frame's bend, then the sweep's steps within the frame.
                writer.SetBend(tick, s.bend);
                for (const SweepStep& step : s.sweeps)
                {
                    writer.Reach(step.tick);
                    if (step.tick < n.off)
                    {
                        writer.SetBend(step.tick, step.bend);
                    }
                }
            }

            writer.Note(n.on, n.off, std::clamp(n.key, 0, 127));
        }
    }

    return midi.Encode();
}

// Runs the model for the conversion of `song` with `settings`, and plans the MIDI file. With `record`, the model
// records a performance too.
Simulation SimulateSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, bool record)
{
    // Start the marked loop on the second pass if it differs from the first. The intro may leave a different octave or
    // other setting from the end of the loop. A channel may continue changing on later passes; the marker still starts
    // at the second. Compare notes on the beat as in the default MIDI output. `frame_timing` then moves the same
    // loops, and not also those that differ only through rounding to frames. Also move the marker if a note at the
    // loop's start would play twice, under either timing mode.
    const Timing timing = settings.frame_timing ? Timing::kFrames : Timing::kBeat;
    Simulation sim = Simulate(rom, info, song, settings.loops, timing, LoopStart::kFirstPass, record);
    if (sim.plan.loops)
    {
        // The checks time one run of three passes twice: on the beat, then by frames.
        Simulation check = RunModel(rom, info, song, 3);
        PlanLoop(check, Timing::kBeat, 3, LoopStart::kFirstPass);
        bool repeats = FirstPassRepeats(rom, info, check) && SeamRepeats(rom, info, check);
        if (repeats)
        {
            PlanLoop(check, Timing::kFrames, 3, LoopStart::kFirstPass);
            repeats = SeamRepeats(rom, info, check);
        }

        if (!repeats)
        {
            sim = Simulate(rom, info, song, settings.loops, timing, LoopStart::kSecondPass, record);
        }
    }

    return sim;
}

// Converts a song, writing its files if `write` is set: to `files` when it isn't null, and otherwise to the paths that
// `settings` gives.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, bool write, SongFiles* files = nullptr)
{
    SongReport report;
    if (!SongAddress(info, song))
    {
        report.warnings.push_back("its header can't be read");
        return report;
    }

    const Simulation sim = SimulateSong(rom, info, song, settings, false);
    report.warnings = sim.warnings;

    NoteMaker maker(rom, info, sim);
    std::array<std::vector<Note>, kChannels> notes;
    for (int c = 0; c < kChannels; c++)
    {
        if (settings.track_mask & (1 << c))
        {
            notes[size_t(c)] = maker.Make(c);
            report.tracks += notes[size_t(c)].empty() ? 0 : 1;
        }
    }

    report.seconds = TickSeconds(sim.frame_ticks, sim.plan.end);
    if (sim.plan.loops)
    {
        report.loop_start = TickSeconds(sim.frame_ticks, sim.plan.loop_start);
        report.loop_end = TickSeconds(sim.frame_ticks, sim.plan.loop_end);
    }

    report.bpm = 60.0 / FrameSeconds(double(kTicksPerQuarter) / TempoMap(sim).front().second);

    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The SoundFont's presets, in the song's file or the shared one, in the banks and programs that it gives the song.
    // In a SoundFont for this song alone, the presets go in song 0's banks and programs.
    SoundfontBuilder own(rom, 1, PsgRate(info.revision));
    SoundfontBuilder& sf = shared ? *shared : own;
    const int count = int(maker.Programs().size());
    const int number = info.first_song + song;
    const std::vector<BankProgram> slots = sf.Banks().Place(shared ? number : 0, count);
    if (int(slots.size()) < count)
    {
        report.warnings.push_back("needs " + std::to_string(count) +
                                  " presets, more than the SoundFont's banks have free" +
                                  (shared ? "; convert the song without --single-sf2" : ""));
        return report;
    }

    for (const std::string& w : AddPresets(rom, info, sf, maker, slots))
    {
        report.warnings.push_back(w);
    }

    const std::string title = rom.Title() + " #" + SongNumber(number, 0);
    char about[160];
    const int effect = EffectIndex(info, song);
    if (effect >= 0)
    {
        std::snprintf(about, sizeof about, "%s (%s) sound effect %d, header at 0x%08X, converted by %s",
                      rom.Title().c_str(), rom.GameCode().c_str(), effect, unsigned(SongAddress(info, song)),
                      settings.program.c_str());
    }
    else
    {
        std::snprintf(about, sizeof about, "%s (%s) song %d, header at 0x%08X, converted by %s", rom.Title().c_str(),
                      rom.GameCode().c_str(), song, unsigned(SongAddress(info, song)), settings.program.c_str());
    }

    const std::string stem = SongStem(settings, SongNumber(number, 0));

    // The song's SoundFont, unless the instruments go in a shared one.
    Sf2File& file = sf.File();
    if (!shared)
    {
        file.program = settings.program;
        file.SetTitle(title);
    }

    report.ok = agb::WriteModuleFiles(stem, MidiBytes(title, about, sim, notes, slots), shared ? nullptr : &file,
                                      report, files ? &files->midi : nullptr, files ? &files->sf2 : nullptr);

    return report;
}

} // namespace

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    SongFiles files;
    files.report = Run(rom, info, song, settings, nullptr, true, &files);

    return files;
}

ConvertedSong ConvertedNotes(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    ConvertedSong converted;
    if (!SongAddress(info, song))
    {
        converted.warnings.push_back("its header can't be read");
        return converted;
    }

    // The steps of Run().
    const Simulation sim = SimulateSong(rom, info, song, settings, true);
    const Plan& plan = sim.plan;
    converted.valid = true;
    converted.warnings = sim.warnings;
    converted.truncated = sim.cut_off;
    converted.ticks_per_quarter = kTicksPerQuarter;
    converted.loops = plan.loops;
    converted.loop_start = plan.loop_start;
    converted.loop_end = plan.loop_end;
    converted.end = plan.end;
    converted.seconds = TickSeconds(sim.frame_ticks, plan.end);
    for (const auto& [tick, tempo] : TempoMap(sim))
    {
        converted.tempos.emplace_back(tick, QuarterMicros(kTicksPerQuarter, uint64_t(tempo)));
    }

    for (size_t i = 0; i < sim.events.size(); i++)
    {
        const Event& e = sim.events[i];
        if (e.kind == Event::kStretch)
        {
            const TrackStretch& s = sim.stretches[size_t(e.index)];
            converted.stretches.push_back({s.channel, RunTick(sim, i), s.first, s.last, s.depth, s.loop});
        }
        else if (e.kind == Event::kVolume)
        {
            const uint32_t tick = RunTick(sim, i);
            if (tick < plan.end)
            {
                converted.volumes.push_back({e.channel, tick, e.index});
            }
        }
    }

    NoteMaker maker(rom, info, sim);
    for (int c = 0; c < kChannels; c++)
    {
        if (settings.track_mask & (1 << c))
        {
            for (const Note& n : maker.Make(c))
            {
                converted.notes.push_back({c, n.command, n.on, n.off, std::clamp(n.key, 0, 127), n.program});
            }
        }
    }

    return converted;
}

} // namespace agblib::sequence_export::quintet
