// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/konami/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert_internal.h"
#include "convert_presets.h"
#include "convert_tracks.h"
#include "converted_notes.h"

namespace agblib::sequence_export::konami
{
namespace
{

constexpr int kVelocity = 127; // every note's velocity. CC11 sets the loudness (see LevelsToControllers()).

// Writes the conductor track: the title and the tempo map, the game and song it's from, its loop, and the driver's echo
// settings. MIDI can't express the echo. `named` follows the song's number: a space and the version's name, or nothing.
// `ticks` gives the grid's tempos and the ticks of frames. The track's events at tick 0 come in another order than
// WriteBeatConductor()'s. That function would change the files.
void WriteConductor(MidiTrack& conductor, const std::string& title, const std::string& named, const BeatTicks& ticks,
                    const Rom& rom, const DriverInfo& info, int song, const Simulation& sim, const char* program)
{
    conductor.Name(title);
    for (const BeatGrid::Tempo& t : ticks.Grid().Tempos())
    {
        conductor.Tempo(QuarterTick(t.quarter), uint32_t(std::lround(FrameSeconds(t.frames) * 1e6)));
    }

    conductor.TimeSignature(0, 4, 2);

    SongHeader h;
    ReadSongHeader(rom, info.song_table, song, info.revision, h);
    char text[192];
    std::snprintf(text, sizeof text, "%s (%s) song %d%s, sequence data at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, named.c_str(), h.base, program);
    conductor.Meta(0, 0x01, text);

    // The beat's tempo at the loop's start is given again after the marker. The loop's end can have another tempo. A
    // player that keeps its tempo when it jumps back would otherwise play the loop's start at that tempo.
    if (sim.loop_end >= 0)
    {
        conductor.Meta(ticks.Marker(sim.loop_start), 0x06, "loopStart");
        conductor.Meta(ticks.Marker(sim.loop_end), 0x06, "loopEnd");
        conductor.RepeatTempo(ticks.Marker(sim.loop_start));
    }

    int last_fb = -1, last_delay = -1;
    for (uint32_t f = 0; f < sim.state.size(); f++)
    {
        const FrameState& e = sim.state[f];
        if (e.feedback == last_fb && e.delay == last_delay)
        {
            continue;
        }

        if (e.feedback || last_fb > 0)
        {
            std::snprintf(text, sizeof text, "echo: delay %.0f ms, feedback %d/256 (sent as CC91 reverb)",
                          e.delay * 32 * 1000.0 / info.mix_rate, e.feedback);
            conductor.Meta(ticks.Song(f), 0x01, text);
        }

        last_fb = e.feedback;
        last_delay = e.delay;
    }

    conductor.SetEnd(ticks.Song(double(sim.frames.size())));
}

// Writes one track's events on MIDI channel `ch`, sending program changes and controllers where they change, and where
// each first comes from `loop`, the loop's start, on. `sound_of` gives the preset and key of a note-on event, `range`
// is the pitch bend range in semitones, and `tick_of` gives a frame's tick on the track.
void WriteTrack(MidiTrack& mt, int ch, int range, const std::vector<Event>& events, std::optional<uint32_t> loop,
                const std::function<NoteSound(const Event&)>& sound_of, const std::function<uint32_t(double)>& tick_of)
{
    // The first note's preset is selected before anything plays, and the controllers start reset.
    BankProgram program;
    for (const Event& e : events)
    {
        if (e.type == Event::kNoteOn)
        {
            program = sound_of(e).program;
            break;
        }
    }

    mt.Bank(0, ch, program.bank);
    mt.Program(0, ch, program.program);
    mt.Control(0, ch, cc::kVolume, 127);
    mt.Control(0, ch, cc::kPan, 64);
    mt.Control(0, ch, cc::kExpression, 0);
    mt.Control(0, ch, cc::kReverb, 0);
    mt.Control(0, ch, cc::kChorus, 0);

    mt.BendRange(0, ch, range);
    mt.PitchBend(0, ch, 8192);

    // The channel's settings, the echo that the track's events last gave, and the key that's playing (-1 if none).
    int cc10 = 64, cc11 = 0, cc91 = 0, bend = 8192, echo = 0, key = -1;

    auto set_levels = [&](uint32_t tick, double l, double r)
    {
        int n10 = cc10, n11 = cc11;
        LevelsToControllers(l, r, n10, n11);
        if (n10 != cc10)
        {
            mt.Control(tick, ch, cc::kPan, cc10 = n10);
        }

        if (n11 != cc11)
        {
            mt.Control(tick, ch, cc::kExpression, cc11 = n11);
        }
    };

    auto set_bend = [&](uint32_t tick, int dev)
    {
        const int v = FixedBendValue(dev, 32, range);
        if (v != bend)
        {
            mt.PitchBend(tick, ch, bend = v);
        }
    };

    for (const Event& e : events)
    {
        const uint32_t tick = tick_of(e.frame);

        // A player that jumps back to the loop's start keeps the settings that the loop's end left. They're therefore
        // all written again from there.
        if (loop && tick >= *loop)
        {
            loop.reset();
            program = {-1, -1};
            cc10 = cc11 = cc91 = bend = -1;
        }

        switch (e.type)
        {
        case Event::kNoteOff:
            if (key >= 0)
            {
                mt.NoteOff(tick, ch, key);
            }

            key = -1;
            break;

        case Event::kNoteOn:
            {
                if (key >= 0)
                {
                    mt.NoteOff(tick, ch, key);
                }

                const NoteSound sound = sound_of(e);
                if (sound.program.bank != program.bank)
                {
                    mt.Bank(tick, ch, sound.program.bank);
                    program = {sound.program.bank, -1};
                }

                if (sound.program.program != program.program)
                {
                    mt.Program(tick, ch, sound.program.program);
                    program.program = sound.program.program;
                }

                set_levels(tick, e.level_l, e.level_r);
                set_bend(tick, e.dev);
                if (echo != cc91)
                {
                    mt.Control(tick, ch, cc::kReverb, cc91 = echo);
                }

                mt.NoteOn(tick, ch, sound.key, kVelocity);
                key = sound.key;
                break;
            }

        case Event::kLevels:
            set_levels(tick, e.level_l, e.level_r);
            break;

        case Event::kBend:
            set_bend(tick, e.dev);
            break;

        case Event::kEcho:
            echo = e.echo;
            if (echo != cc91)
            {
                mt.Control(tick, ch, cc::kReverb, cc91 = echo);
            }
            break;
        }
    }
}

// Moves the end of a song without a loop back to where its last sound dies away, when the end is more than
// kMaxSilentEndSeconds later as MP2K's conversion does. A track sounds from a note with a level on either side until
// the note ends or its levels fall to 0. The song then ends two frames after the last such end. On the beat, a note's
// end can be up to a frame and a half after its frame, and the second frame keeps it in the file. A note that still
// sounds where the song stops keeps the end, and so does a song that reached the frame limit.
void EndWithLastSound(const Rom& rom, const DriverInfo& info, Simulation& sim)
{
    if (sim.loop_end >= 0 || sim.truncated || sim.frames.empty())
    {
        return;
    }

    std::vector<std::string> warnings;
    const std::array<std::vector<Event>, kTracks> events = RenderTracks(rom, info, sim, warnings);
    uint32_t last = 0; // the frame of the last sound's end
    for (const std::vector<Event>& track : events)
    {
        bool on = false, sounds = false;
        for (const Event& e : track)
        {
            const bool level = e.level_l > 0 || e.level_r > 0;
            bool now = sounds;
            if (e.type == Event::kNoteOn)
            {
                on = true;
                now = level;
            }
            else if (e.type == Event::kLevels && on)
            {
                now = level;
            }
            else if (e.type == Event::kNoteOff)
            {
                on = false;
                now = false;
            }

            if (sounds && !now)
            {
                last = std::max(last, e.frame);
            }

            sounds = now;
        }
    }

    const uint32_t end = uint32_t(sim.frames.size());
    const uint32_t quiet = last + 2;
    if (last > 0 && quiet < end && FrameSeconds(end - quiet) > kMaxSilentEndSeconds)
    {
        sim.frames.resize(quiet);
        sim.state.resize(quiet);
    }
}

// Simulates the song for its conversion. The marked loop starts at the song's second pass if the first pass plays
// unlike the second. That happens, for example, when the tracks come back to their loop points with the volume or pan
// that the loop's end left them. A delay at the start of a loop point plays only in the later passes. It moves its
// track's notes there. The comparison of passes times each track's notes from its first note and misses the move. A
// song without a loop ends where its last sound dies away (EndWithLastSound()).
Simulation MarkedSimulation(const Rom& rom, const DriverInfo& info, int song, const Request& request, int loops)
{
    Simulation sim = Simulate(rom, info, song, request, loops, LoopStart::kFirstPass);
    if (sim.loop_end >= 0 &&
        (sim.delayed_loop_point ||
         !FirstPassRepeats(rom, info, Simulate(rom, info, song, request, 2, LoopStart::kFirstPass))))
    {
        sim = Simulate(rom, info, song, request, loops, LoopStart::kSecondPass);
    }

    EndWithLastSound(rom, info, sim);

    return sim;
}

// Copies the presets that track `t` plays in `events` into the drum bank. Some players force channel 10 to that bank.
void CopyDrumPresets(int t, const std::vector<Event>& events, const Programs& programs, SoundfontBuilder& sf)
{
    std::set<std::pair<int, int>> used; // (bank, program) of each of the track's presets
    for (const Event& e : events)
    {
        if (e.type == Event::kNoteOn)
        {
            const BankProgram p = Resolve(programs, sf, t, e).program;
            used.insert({p.bank, p.program});
        }
    }

    std::vector<sf2::Preset>& presets = sf.File().presets;
    const size_t count = presets.size();
    for (size_t i = 0; i < count; i++)
    {
        if (used.count({presets[i].bank, presets[i].program}))
        {
            sf2::Preset copy = presets[i];
            copy.bank = kDrumBank;
            presets.push_back(copy);
        }
    }
}

// Returns the song's beat. The beat comes from the song's start and from the frames on which every track's notes start,
// whichever tracks the conversion includes. Conversions of different tracks then line up. Each pass through the loop
// starts again on the beat. The song ends at frame `end_frame`.
BeatGrid SongGrid(const std::array<std::vector<Event>, kTracks>& events, const Simulation& sim, uint32_t end_frame)
{
    std::vector<std::vector<uint32_t>> starts(kTracks);
    for (int t = 0; t < kTracks; t++)
    {
        for (const Event& e : events[size_t(t)])
        {
            if (e.type == Event::kNoteOn)
            {
                starts[size_t(t)].push_back(e.frame);
            }
        }

        if (!starts[size_t(t)].empty())
        {
            starts[size_t(t)].insert(starts[size_t(t)].begin(), 0);
        }
    }

    std::vector<uint32_t> hints;
    const int length = sim.loop_end - sim.loop_start;
    for (int f = sim.first_loop_end; length > 0 && f < int(end_frame); f += length)
    {
        hints.push_back(uint32_t(f));
    }

    return BeatGrid(starts, hints);
}

// Writes the song's MIDI file to the folder and under the base name in `settings`, with the version's suffix
// `suffix`. Writes the SoundFont `sf` there too, titled `title`, unless `sf` is null. Adds each file's path to
// `report`. Sets report.ok if every file is written. Otherwise adds the error to report.warnings. With `files`, the
// files' bytes go there instead, with the SFZ files of `tracks`, and the report names no paths. SFZ files that can't
// be made leave `files->sfz` empty, with a warning.
void WriteSongFiles(const ConvertSettings& settings, const std::string& number, const std::string& suffix,
                    const std::string& title, const MidiFile& midi, SoundfontBuilder* sf,
                    std::span<const SfzTrack> tracks, SongReport& report, SongFiles* files)
{
    std::string error;
    if (files)
    {
        files->midi = midi.Encode();
        if (sf)
        {
            Sf2File& file = sf->File();
            file.program = settings.program;
            file.SetTitle(title);
            if (!file.Encode(files->sf2, error))
            {
                report.warnings.push_back(error);
                return;
            }

            try
            {
                files->sfz = agblib::sequence_export::agb::TrackSfz(file, tracks, title, settings.program);
            }
            catch (const std::invalid_argument& e)
            {
                report.warnings.push_back(std::string("no SFZ files: ") + e.what());
            }
        }

        report.ok = true;
        return;
    }

    const std::string stem = SongStem(settings, number) + suffix;
    report.midi_path = stem + ".mid";
    if (!WriteFile(report.midi_path, midi.Encode(), error))
    {
        report.warnings.push_back(error);
        return;
    }

    if (sf)
    {
        Sf2File& file = sf->File();
        file.program = settings.program;
        file.SetTitle(title);
        report.sf2_path = stem + ".sf2";
        if (!file.Write(report.sf2_path, error))
        {
            report.warnings.push_back(error);
            return;
        }
    }

    report.ok = true;
}

// The SFZ file's view of MIDI track `name`, whose bend range is `range` semitones and whose notes are the note-on
// events of `events`: the presets of `file` that `sound_of` picks for the notes. The drum bank's copies stay out.
SfzTrack SfzTrackOf(const std::string& name, int range, const std::vector<Event>& events,
                    const std::function<NoteSound(const Event&)>& sound_of, const Sf2File& file)
{
    SfzTrack track{.name = name, .bend_range = range};
    std::set<std::pair<int, int>> selected;
    for (const Event& e : events)
    {
        if (e.type == Event::kNoteOn)
        {
            const BankProgram program = sound_of(e).program;
            selected.insert({program.bank, program.program});
        }
    }

    for (size_t p = 0; p < file.presets.size(); p++)
    {
        if (selected.contains({file.presets[p].bank, file.presets[p].program}))
        {
            track.presets.push_back(p);
        }
    }

    return track;
}

// Returns the state in which a song's version plays. The live stage's version plays in the state that the live stage
// leaves: FA's setting at 0, and F6's table of samples where the song has one.
Request RequestOf(const DriverInfo& info, int song, size_t version)
{
    Request request;
    if (version > 0)
    {
        request.setting = kLiveStageSetting;
        if (const auto it = info.live_tables.find(song); it != info.live_tables.end())
        {
            request.samples = it->second;
        }
    }

    return request;
}

// Converts a song, writing its files if `write` is set. With `files`, the files' bytes go there instead, and the report
// names no paths.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, bool write, SongFiles* files = nullptr)
{
    const std::vector<SongVersion> versions = SongVersions(info, song);
    const size_t version = size_t(std::clamp(settings.version, 0, int(versions.size()) - 1));
    const Request request = RequestOf(info, song, version);

    SongReport report;
    const Simulation sim = MarkedSimulation(rom, info, song, request, settings.loops);
    report.warnings = sim.warnings;
    if (sim.frames.empty())
    {
        return report;
    }

    const uint32_t end_frame = uint32_t(sim.frames.size());
    report.seconds = FrameSeconds(end_frame);
    if (sim.loop_end >= 0)
    {
        report.loop_start = FrameSeconds(sim.loop_start);
        report.loop_end = FrameSeconds(sim.loop_end);
    }

    const std::array<std::vector<Event>, kTracks> events = RenderTracks(rom, info, sim, report.warnings);
    const Usage usage = CollectUsage(events, settings.track_mask);
    report.tracks = int(std::count(usage.used.begin(), usage.used.end(), true));
    report.silent = report.tracks == 0; // placeholder entries, such as a "no music" song, play no notes at all
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // With a SoundFont shared by all songs, SongBanks gives each song's presets their bank and programs.
    std::unique_ptr<SoundfontBuilder> own;
    SoundfontBuilder& sf = shared ? *shared : *(own = std::make_unique<SoundfontBuilder>(rom, info));
    const SoundfontBuilder::Checkpoint before = sf.Save();
    const std::string number = SongNumber(song, 0);
    const std::string prefix = shared ? "S" + number + " " : "";
    const Programs programs = AddPresets(usage, sf, rom, info, shared ? song : 0, prefix);
    if (!programs.fits)
    {
        const size_t preset_count = sf.File().presets.size() - before.presets;
        sf.Restore(before);
        report.warnings.push_back("needs " + std::to_string(preset_count) +
                                  " presets, more than the SoundFont's banks have free" +
                                  (shared ? "; convert the song without --single-sf2" : ""));
        return report;
    }

    // Some players force channel 10 to the drum bank. The presets of the track on it therefore get a copy there, to be
    // found either way. Copies of every preset would clash when there are more than 128.
    const std::array<int, kTracks> channel = AssignChannels(usage.used);
    const auto drum_channel = std::find(channel.begin(), channel.end(), kDrumChannel);
    if (drum_channel != channel.end())
    {
        if (shared)
        {
            report.warnings.push_back("a 16th channel is needed; channel 10 may play from the drum bank");
        }
        else
        {
            const int t = int(drum_channel - channel.begin());
            CopyDrumPresets(t, events[size_t(t)], programs, sf);
        }
    }

    const BeatGrid grid = SongGrid(events, sim, end_frame);
    report.bpm = 60 / FrameSeconds(grid.Tempos().front().frames);

    // Each event goes on the beat, or on the driver's frame, up to the song's end.
    const BeatTicks ticks(grid, settings.frame_timing);
    const uint32_t end_tick = ticks.Song(end_frame);

    // The loop's start, from which each track writes its settings again, unless the loop starts with the song.
    const uint32_t loop_start = sim.loop_end >= 0 ? ticks.Marker(sim.loop_start) : 0;
    const std::optional<uint32_t> loop = loop_start > 0 ? std::optional(loop_start) : std::nullopt;

    // A version after the first has its name in the title and its suffix in the files' names.
    MidiFile midi(kBeatQuarterTicks);
    const std::string named = versions[version].name.empty() ? "" : " " + versions[version].name;
    const std::string title = rom.Title() + " #" + number + named;
    WriteConductor(midi.AddTrack(), title, named, ticks, rom, info, song, sim, settings.program.c_str());

    std::vector<SfzTrack> sfz_tracks;
    for (int t = 0; t < kTracks; t++)
    {
        if (!usage.used[size_t(t)])
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        mt.Name(TrackName(t));

        // The range fits the track's largest bend. In the Eternal Duelist revision, a PSG note at the volume that's
        // playing only changes the channel's frequency, and a run of them can take a bend past 24 semitones.
        const int range = std::clamp((usage.max_dev[size_t(t)] + 31) / 32, 2, 127);
        auto sound_of = [&](const Event& e)
        {
            return Resolve(programs, sf, t, e);
        };

        const auto tick_of = [&](double frame)
        {
            return ticks.Channel(t, frame, end_tick);
        };
        WriteTrack(mt, channel[size_t(t)], range, events[size_t(t)], loop, sound_of, tick_of);
        mt.SetEnd(end_tick);
        sfz_tracks.push_back(SfzTrackOf(TrackName(t), range, events[size_t(t)], sound_of, sf.File()));
    }

    WriteSongFiles(settings, number, versions[version].suffix, title, midi, shared ? nullptr : &sf, sfz_tracks, report,
                   files);

    return report;
}

} // namespace

ConvertedSong ConvertedNotes(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    const std::vector<SongVersion> versions = SongVersions(info, song);
    const size_t version = size_t(std::clamp(settings.version, 0, int(versions.size()) - 1));
    const Request request = RequestOf(info, song, version);

    ConvertedSong converted;
    const Simulation sim = MarkedSimulation(rom, info, song, request, settings.loops);
    converted.warnings = sim.warnings;
    converted.truncated = sim.truncated;
    if (sim.frames.empty())
    {
        return converted;
    }

    // The reads of the run that the conversion plays.
    Simulate(rom, info, song, request, settings.loops, sim.start_pass, &converted.reads);

    const uint32_t end_frame = uint32_t(sim.frames.size());
    converted.end_frame = end_frame;
    converted.loops = sim.loop_end >= 0;
    if (converted.loops)
    {
        converted.loop_start = uint32_t(sim.loop_start);
        converted.loop_end = uint32_t(sim.loop_end);
    }

    const std::array<std::vector<Event>, kTracks> events = RenderTracks(rom, info, sim, converted.warnings);
    const Usage usage = CollectUsage(events, settings.track_mask);
    if (std::count(usage.used.begin(), usage.used.end(), true) == 0)
    {
        return converted;
    }

    SoundfontBuilder sf(rom, info);
    const Programs programs = AddPresets(usage, sf, rom, info, 0, "");
    if (!programs.fits)
    {
        converted.warnings.push_back("the song takes more presets than the SoundFont's banks have free");
        return converted;
    }

    const BeatGrid grid = SongGrid(events, sim, end_frame);
    converted.quarter_frames = grid.Tempos().front().frames;
    const BeatTicks ticks(grid, settings.frame_timing);
    converted.end_tick = ticks.Song(end_frame);

    for (int t = 0; t < kTracks; t++)
    {
        if (!usage.used[size_t(t)])
        {
            continue;
        }

        const auto tick_of = [&](uint32_t frame)
        {
            return ticks.Channel(t, frame, converted.end_tick);
        };

        std::optional<size_t> playing;
        const auto end_note = [&](uint32_t frame)
        {
            if (playing)
            {
                converted.notes[*playing].off_frame = frame;
                converted.notes[*playing].off_tick = tick_of(frame);
            }

            playing.reset();
        };

        for (const Event& e : events[size_t(t)])
        {
            if (e.type == Event::kNoteOff)
            {
                end_note(e.frame);
            }
            else if (e.type == Event::kNoteOn)
            {
                end_note(e.frame);
                const NoteSound sound = Resolve(programs, sf, t, e);
                ConvertedNote note;
                note.track = t;
                note.on_frame = e.frame;
                note.on_tick = tick_of(e.frame);
                note.key = sound.key;
                note.velocity = kVelocity;
                note.bank = sound.program.bank;
                note.program = sound.program.program;
                playing = converted.notes.size();
                converted.notes.push_back(note);
            }
        }
    }

    return converted;
}

SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    SongFiles files;
    files.report = Run(rom, info, song, settings, nullptr, true, &files);

    return files;
}

std::vector<SongVersion> SongVersions(const DriverInfo& info, int song)
{
    if (std::find(info.live_songs.begin(), info.live_songs.end(), song) == info.live_songs.end())
    {
        return {SongVersion()};
    }

    return {SongVersion(), {"in the live stage", "_live"}};
}

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    SongHeader h;
    if (!ReadSongHeader(rom, info.song_table, song, info.revision, h))
    {
        error = "invalid song";
        return false;
    }

    std::string text;
    char line[256];
    std::snprintf(line, sizeof line, "; %s (%s) song %d\n; header 0x%08X, sequence base 0x%08X\n", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, info.song_table + uint32_t(song) * SongEntrySize(info.revision),
                  h.base);
    text += line;
    text += "; columns: address, frame, bytes, command, then the delay that follows it\n";

    for (int t = 0; t < h.tracks; t++)
    {
        std::snprintf(line, sizeof line, "\n; ---- track %d (%s), offset 0x%04X -> 0x%08X ----\n", t,
                      TrackName(t).c_str(), h.offsets[t], h.base + h.offsets[t]);
        text += line;

        // Each command's line is finished by the delay that follows it. The commands that a call plays come after the
        // call's line. The frames after the call count their delays.
        std::string pending;
        auto list_command = [&](const Command& c, uint32_t frame)
        {
            if (c.op == Op::kDelay)
            {
                if (pending.empty())
                {
                    std::snprintf(line, sizeof line, "0x%08X  %6u  %-14s  %-44s wait %u\n", c.addr, frame, "",
                                  "(start)", c.delay);
                    text += line;
                }
                else
                {
                    text += pending + " wait " + std::to_string(c.delay) + "\n";
                    pending.clear();
                }

                return;
            }

            const std::string bytes = HexBytes(rom, c.addr, c.length);
            std::snprintf(line, sizeof line, "0x%08X  %6u  %-14s  %-44s", c.addr, frame, bytes.c_str(),
                          Describe(c, t, info.revision).c_str());
            pending = line;
            if (c.op == Op::kEndTrack || c.op == Op::kEndTrackHard || c.op == Op::kJump || c.op == Op::kUnknown)
            {
                text += pending + "\n";
                pending.clear();
            }
        };
        WalkTrack(rom, h, t, info.revision, list_command, kMaxWalkCommands, true);

        if (!pending.empty())
        {
            text += pending + "\n";
        }
    }

    return WriteTextFile(path, text, error);
}

} // namespace agblib::sequence_export::konami
