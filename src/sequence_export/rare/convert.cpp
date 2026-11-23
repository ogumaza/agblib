// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/rare/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"

#include "agblib/sequence_export/agb/module_conversion.h"
#include "convert_internal.h"
#include "convert_notes.h"
#include "convert_simulation.h"
#include "converted_notes.h"

namespace agblib::sequence_export::rare
{
namespace
{

// Maximum number of commands DumpSong() lists per track.
constexpr int kMaxListedCommands = 200000;

// Returns the General MIDI 2 Controller Destination Setting message (MMA CA-022) for MIDI channel `ch`: a universal
// real time message (7F) to every device (7F), of the Controller Destination Setting (09) for polyphonic key pressure
// (02), on the channel, that sets amplitude control (02) to a range of 0. A key's notes then play at their full level
// at pressure 0 and fall silent at 127.
std::vector<uint8_t> KeyPressureMuteMessage(int ch)
{
    return {0xF0, 0x7F, 0x7F, 0x09, 0x02, uint8_t(ch), 0x02, 0x00, 0xF7};
}

// Writes a channel's settings at the start: the driver's volume (unless the tune sets one at the start), no pan or
// effects, the bend range, and the game's music volume, out of 128 as expression. The mixer scales every note by the
// music volume. Expression can't go above 127. A music volume above 128 plays at 128.
void WriteChannelSetup(MidiTrack& mt, int ch, int range, int bank, bool volume_at_start, int music_volume)
{
    if (bank)
    {
        mt.Bank(0, ch, bank);
    }

    if (!volume_at_start)
    {
        mt.Control(0, ch, cc::kVolume, 127);
    }

    mt.Control(0, ch, cc::kPan, 64);
    mt.Control(0, ch, cc::kExpression, SquareRootValue(std::min(music_volume, 128), 128, 0));
    mt.Control(0, ch, cc::kReverb, 0);
    mt.Control(0, ch, cc::kChorus, 0);
    if (range > 0)
    {
        mt.BendRange(0, ch, range);
        mt.PitchBend(0, ch, 8192);
    }
}

// The programs whose presets AddPresets() couldn't add as the notes play them.
struct PresetProblems
{
    std::set<int> missing; // the programs whose instruments have nothing to play
    std::set<int> clashes; // the drum channel's programs that another set of instruments has in the drum bank
};

// Adds a preset for each program the notes play, in each note's bank. A note on the drum channel without envelope
// settings gets a copy in the drum bank too. In a shared SoundFont, another set of instruments can have the program
// there already.
PresetProblems AddPresets(SoundfontBuilder& sf, const std::vector<Note>& notes)
{
    PresetProblems problems;
    for (const Note& n : notes)
    {
        const int instrument = sf.InstrumentFor(n.instrument, n.envelope);
        if (instrument < 0)
        {
            problems.missing.insert(n.program);
            continue;
        }

        sf.AddPreset(n.bank, n.program, instrument);
        if (n.channel == kDrumChannel && n.envelope.None())
        {
            const sf2::Preset* preset = sf.FindPreset(kDrumBank, n.program);
            const int drum = preset ? PresetInstrument(*preset) : -1;
            if (drum < 0)
            {
                sf.AddPreset(kDrumBank, n.program, instrument);
            }
            else if (drum != instrument)
            {
                problems.clashes.insert(n.program);
            }
        }
    }

    return problems;
}

// A program change, with the bank it chooses, and the track of the MIDI file it goes in, given by the tune's track
// number.
struct ProgramChange
{
    uint64_t tick;
    int track;
    int channel;
    int program;
    int bank;
};

// Preserves the driver's program for each note despite differences in event ordering. A player takes a tick's events
// track by track, and within a track, program changes before note-ons, where the driver runs each track's commands of a
// frame in order. So a program change on a note's channel and tick, after the note in its track or on an earlier track
// in the file, would give the note the wrong program. Such a note moves before its track's program changes on its tick,
// or gets a program change of its own. So does a note whose preset is in another bank than the program change before it
// chooses, for the envelope settings it plays with.
void MatchPrograms(std::vector<Note>& notes, std::vector<ProgramChange>& programs)
{
    auto in_file_order = [](const ProgramChange& a, const ProgramChange& b)
    {
        return a.tick != b.tick ? a.tick < b.tick : a.track < b.track;
    };
    std::stable_sort(programs.begin(), programs.end(), in_file_order);

    std::vector<size_t> order(notes.size());
    for (size_t i = 0; i < order.size(); i++)
    {
        order[i] = i;
    }

    auto note_order = [&](size_t a, size_t b)
    {
        return notes[a].on != notes[b].on ? notes[a].on < notes[b].on : notes[a].track < notes[b].track;
    };
    std::stable_sort(order.begin(), order.end(), note_order);

    for (size_t i : order)
    {
        // The bank and program the file gives the note, and those it would give it before its track's changes on its
        // tick.
        Note& n = notes[i];
        std::pair<int, int> before = {-1, -1}, after = {-1, -1};
        for (const ProgramChange& p : programs)
        {
            if (p.tick > n.on || (p.tick == n.on && p.track > n.track))
            {
                break;
            }
            if (p.channel != n.channel)
            {
                continue;
            }

            after = {p.bank, p.program};
            if (p.tick < n.on || p.track < n.track)
            {
                before = after;
            }
        }

        const std::pair<int, int> preset = {n.bank, n.program};
        if (n.program < 0 || preset == after)
        {
            continue;
        }

        if (preset == before)
        {
            n.before_programs = true;
            continue;
        }

        const ProgramChange change = {n.on, n.track, n.channel, n.program, n.bank};
        programs.insert(std::upper_bound(programs.begin(), programs.end(), change, in_file_order), change);
    }
}

// Writes `programs`. `track` contains the MIDI file's track for each of the tune's tracks. Each channel starts in bank
// `bank`.
void WritePrograms(const std::vector<ProgramChange>& programs, const std::vector<MidiTrack*>& track, int bank)
{
    // A program change gives its bank first where the channel's bank changes, to that of the presets for the envelope
    // settings that the notes after it play with, or back to the tune's.
    std::array<int, kChannels> channel_bank;
    channel_bank.fill(bank);
    for (const ProgramChange& p : programs)
    {
        if (p.bank != channel_bank[size_t(p.channel)])
        {
            track[size_t(p.track)]->Bank(uint32_t(p.tick), p.channel, p.bank);
            channel_bank[size_t(p.channel)] = p.bank;
        }

        track[size_t(p.track)]->Program(uint32_t(p.tick), p.channel, p.program);
    }
}

// Writes the pitch bends of each channel that has a track in `home` and a bend range in `range`.
void WriteBends(const Simulation& sim, const SongPlan& plan, const std::array<MidiTrack*, kChannels>& home,
                const std::array<int, kChannels>& range)
{
    // A player that jumps back to the loop's start keeps the bends that the loop's end left. The game keeps them too,
    // until the loop's first bend or program change on the channel sets its pitch. So each channel's bend is written
    // again at the loop's start. That's left out where the bend changes there or before, or where the loop starts with
    // the tune.
    std::array<uint64_t, kChannels> loop_set;
    loop_set.fill(plan.end);
    for (const Action& a : sim.actions)
    {
        if ((a.kind == Action::kBend || a.kind == Action::kProgram) && a.tick >= plan.loop_start)
        {
            loop_set[size_t(a.channel)] = std::min(loop_set[size_t(a.channel)], a.tick);
        }
    }

    std::array<int64_t, kChannels> loop_offset = {};
    for (const PitchChange& p : sim.pitches)
    {
        if (p.tick < plan.loop_start)
        {
            loop_offset[size_t(p.channel)] = p.offset;
        }
        else if (p.tick <= loop_set[size_t(p.channel)])
        {
            loop_set[size_t(p.channel)] = plan.end;
        }

        if (home[size_t(p.channel)] && range[size_t(p.channel)] > 0 && p.tick < plan.end)
        {
            home[size_t(p.channel)]->PitchBend(uint32_t(p.tick), p.channel,
                                               FixedBendValue(p.offset, kSemitone, range[size_t(p.channel)]));
        }
    }

    for (int ch = 0; ch < kChannels; ch++)
    {
        if (plan.loops && plan.loop_start > 0 && loop_set[size_t(ch)] < plan.end && home[size_t(ch)] &&
            range[size_t(ch)] > 0)
        {
            home[size_t(ch)]->PitchBend(uint32_t(loop_set[size_t(ch)]), ch,
                                        FixedBendValue(loop_offset[size_t(ch)], kSemitone, range[size_t(ch)]));
        }
    }
}

// Returns the MIDI file's bytes: a conductor track, then a track for each of the tune's tracks that plays notes.
std::vector<uint8_t> MidiBytes(const std::string& title, const std::string& about, const TuneHeader& header,
                               const Simulation& sim, const SongPlan& plan, const std::vector<Note>& notes,
                               const std::vector<MuteChange>& pressures, const std::array<int, kChannels>& range,
                               int bank, int music_volume)
{
    MidiFile midi(uint16_t(header.ticks_per_quarter));
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    if (plan.loops)
    {
        conductor.Meta(uint32_t(plan.loop_start), 0x06, "loopStart");
        conductor.Meta(uint32_t(plan.loop_end), 0x06, "loopEnd");
    }

    conductor.SetEnd(uint32_t(plan.end));

    // A track for each of the tune's tracks with notes, in order. A channel's events go in the first one that plays it.
    std::vector<bool> plays(header.track_count, false);
    std::array<int, kChannels> first = {};
    first.fill(int(header.track_count));
    for (const Note& n : notes)
    {
        plays[size_t(n.track)] = true;
        first[size_t(n.channel)] = std::min(first[size_t(n.channel)], n.track);
    }

    std::vector<MidiTrack*> track(header.track_count, nullptr);
    for (uint32_t t = 0; t < header.track_count; t++)
    {
        if (plays[t])
        {
            track[t] = &midi.AddTrack();
            track[t]->Name(seqlib::midi::DefaultTrackName(int(t)));
            track[t]->SetEnd(uint32_t(plan.end));
        }
    }

    std::array<MidiTrack*, kChannels> home = {};
    for (int ch = 0; ch < kChannels; ch++)
    {
        home[size_t(ch)] = first[size_t(ch)] < int(header.track_count) ? track[size_t(first[size_t(ch)])] : nullptr;
    }

    // The tempo goes in the conductor track, at the GBA's speed, on the ticks that TempoChanges() gives. Program
    // changes and volume go in the track that gave them, if it's in the file, or else in the channel's track.
    std::array<bool, kChannels> volume_at_start = {};
    std::vector<ProgramChange> programs;
    uint64_t tempo_tick = 0;
    for (const Action& a : sim.actions)
    {
        if (a.tick > plan.end || (a.tick == plan.end && a.kind != Action::kTempo))
        {
            continue;
        }

        const int destination = track[a.track] ? a.track : first[a.channel];
        MidiTrack* mt = destination < int(header.track_count) ? track[size_t(destination)] : nullptr;
        switch (a.kind)
        {
        case Action::kTempo:
            tempo_tick = std::max(tempo_tick, a.tick);
            conductor.Tempo(uint32_t(tempo_tick), uint32_t(std::lround(a.value * kTempoScale)));
            break;

        case Action::kProgram:
            if (mt)
            {
                programs.push_back({a.tick, destination, a.channel, a.a, bank});
            }
            break;

        case Action::kVolume:
            if (mt)
            {
                mt->Control(uint32_t(a.tick), a.channel, cc::kVolume, SquareRootValue(a.a, 127, 0));
                volume_at_start[a.channel] = volume_at_start[a.channel] || a.tick == 0;
            }
            break;

        default:
            break;
        }
    }

    for (int ch = 0; ch < kChannels; ch++)
    {
        if (home[size_t(ch)])
        {
            WriteChannelSetup(*home[size_t(ch)], ch, range[size_t(ch)], bank, volume_at_start[size_t(ch)],
                              music_volume);
        }
    }

    std::vector<Note> ordered = notes;
    MatchPrograms(ordered, programs);
    WritePrograms(programs, track, bank);
    WriteBends(sim, plan, home, range);

    // A player that ignores the SoundFont's mute modulator can follow the General MIDI 2 Controller Destination Setting
    // message instead. Each channel that mutes a key gets the message at the start, before the channel's key pressures.
    std::array<bool, kChannels> mutes = {};
    for (const MuteChange& m : pressures)
    {
        mutes[size_t(m.channel)] = mutes[size_t(m.channel)] || m.muted;
    }

    for (int ch = 0; ch < kChannels; ch++)
    {
        if (mutes[size_t(ch)])
        {
            home[size_t(ch)]->SysEx(0, KeyPressureMuteMessage(ch));
        }
    }

    // The key pressures go in the channel's track, before the note ons at their ticks.
    for (const MuteChange& m : pressures)
    {
        home[size_t(m.channel)]->KeyPressure(uint32_t(m.tick), m.channel, m.key, m.muted ? 127 : 0);
    }

    for (const Note& n : ordered)
    {
        MidiTrack& mt = *track[size_t(n.track)];
        mt.NoteOn(uint32_t(n.on), n.channel, n.key, SquareRootValue(n.velocity, 127, 1), n.before_programs);
        mt.NoteOff(uint32_t(n.off), n.channel, n.key);
    }

    return midi.Encode();
}

// Returns the number of notes that the driver drops before the plan's end, in the tracks of `track_mask`.
int DroppedNotes(const Simulation& sim, const SongPlan& plan, uint32_t track_mask)
{
    int dropped = 0;
    for (const Action& a : sim.actions)
    {
        dropped += a.kind == Action::kNoteDropped && a.tick < plan.end && (track_mask >> a.track & 1) ? 1 : 0;
    }

    return dropped;
}

// Returns the version of tune `song` that `settings` picks, or the first where the tune has no such version.
SongVersion PickedVersion(const DriverInfo& info, int song, const ConvertSettings& settings)
{
    const std::vector<SongVersion> versions = TuneVersions(info, song);
    return versions[size_t(std::clamp(settings.version, 0, int(versions.size()) - 1))];
}

// Writes the files of tune `song` after Run() has converted it: the MIDI file, and the SoundFont unless the tune uses
// `shared`. With `shared`, the tune's presets go in bank `bank` there. Adds the warnings to `report`. Sets `report.ok`
// once the files are written. With `files`, the files' bytes go there instead, and the report names no paths.
void WriteTune(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, int bank, const TuneHeader& header, const SongPlan& plan,
               const Simulation& sim, std::vector<Note>& notes, SongReport& report, SongFiles* files)
{
    // The SoundFont's presets, in the song's file or the shared one. Those of notes that play with envelope settings go
    // in banks of their own.
    SoundfontBuilder own(rom, info);
    SoundfontBuilder& sf = shared ? *shared : own;
    for (Note& n : notes)
    {
        n.bank = sf.BankFor(bank, n.envelope);
    }

    // The SoundFont mutes a note while its key's pressure is high, in a tune that the mixer leaves voices out of.
    std::vector<MuteChange> pressures = KeyPressures(sim, plan, notes);
    const bool mutes = std::any_of(pressures.begin(), pressures.end(), [](const MuteChange& m) { return m.muted; });
    if (mutes)
    {
        sf.AddMutes();
    }

    // A file that a SoundFont with mutes plays starts with pressure 0 for each key that plays before the tune gives it
    // a pressure. The shared SoundFont can gain the mutes from a later tune. Every file that shares it therefore starts
    // that way too. These pressures come before the tune's pressures at tick 0.
    if (mutes || shared)
    {
        const std::vector<MuteChange> start = StartPressures(notes, pressures);
        pressures.insert(pressures.begin(), start.begin(), start.end());
    }

    const PresetProblems problems = AddPresets(sf, notes);
    for (int program : problems.missing)
    {
        report.warnings.push_back("program " + std::to_string(program) + "'s instrument has no samples to play");
    }

    for (int program : problems.clashes)
    {
        report.warnings.push_back("channel 10's program " + std::to_string(program) +
                                  " is another set of instruments' in the drum bank, which some players use there");
    }

    // The tune's number has two digits, or more from 100 on as Music::FileNumber() gives it. A version after the first
    // has its name in the titles and its suffix in the files' names.
    const std::string number = SongNumber(song, 0);
    const SongVersion version = PickedVersion(info, song, settings);
    const std::string named = version.name.empty() ? "" : " " + version.name;
    const std::string title = rom.Title() + " #" + number + named;
    char about[192];
    std::snprintf(about, sizeof about, "%s (%s) tune %d%s, header at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, named.c_str(), unsigned(header.address), settings.program.c_str());
    const std::string stem = SongStem(settings, number) + version.suffix;

    // The tune's SoundFont, unless the instruments go in a shared one.
    Sf2File& file = sf.File();
    if (!shared)
    {
        file.program = settings.program;
        file.SetTitle(title);
    }

    std::vector<uint8_t> midi = MidiBytes(title, about, header, sim, plan, notes, pressures, BendRanges(rom, sim), bank,
                                          MusicVolume(info, song));
    report.ok = agb::WriteModuleFiles(stem, std::move(midi), shared ? nullptr : &file, report,
                                      files ? &files->midi : nullptr, files ? &files->sf2 : nullptr);
}

// Converts a tune, writing its files if `write` is set. With `shared`, the tune's presets go in bank `bank` there. With
// `files`, the files' bytes go there instead of to files, and the report names no paths.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, int bank, bool write, SongFiles* files = nullptr)
{
    SongReport report;
    TuneHeader header;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, song), header))
    {
        report.warnings.push_back("its header can't be read");
        return report;
    }

    // Each version of the tune that the game plays by the water contains the other version's channels at a level of 0.
    SongPlan plan = PlanTune(rom, info, header, settings.loops);
    const uint16_t silenced = SilencedChannels(info, song, settings.version == kUnderWater);
    const Simulation sim = Simulate(rom, info, song, silenced, header, plan, settings.frame_timing);

    // A tune that the model gave up on ends where it stopped.
    if (sim.cut_off)
    {
        plan.end = std::min(plan.end, *sim.cut_off);
    }

    Summarize(sim, plan, header.ticks_per_quarter, report);

    // With frame timing, the loop's start and end go at the start of the frames that play them as their events do.
    // Each pass's first events then fall on the same side of the marker. The end goes at the start of the first frame
    // that reaches it, after the frame that plays it.
    if (settings.frame_timing)
    {
        plan.loop_start = PlayingFrame(sim.actions, header.ticks_per_quarter, sim.frame_starts, plan.loop_start);
        plan.loop_end = PlayingFrame(sim.actions, header.ticks_per_quarter, sim.frame_starts, plan.loop_end);
        plan.end = OnFrame(sim.frame_starts, plan.end);
    }

    report.warnings = sim.warnings;
    std::vector<Note> notes =
        NoteCollector(sim, plan, settings.track_mask, kChannels * info.slots_per_channel).Collect();

    std::set<int> tracks;
    for (const Note& n : notes)
    {
        tracks.insert(n.track);
    }

    const int dropped = DroppedNotes(sim, plan, settings.track_mask);
    if (dropped)
    {
        report.warnings.push_back(
            std::to_string(dropped) + " note" + (dropped == 1 ? "" : "s") +
            " found no available slot on their channel; omitted from the MIDI file to match the game");
    }

    report.tracks = int(tracks.size());
    report.silent = notes.empty();
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    WriteTune(rom, info, song, settings, shared, bank, header, plan, sim, notes, report, files);

    return report;
}

} // namespace

ConvertedTune ConvertedNotes(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    ConvertedTune tune;
    TuneHeader header;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, song), header))
    {
        tune.warnings.push_back("its header can't be read");
        return tune;
    }

    // The steps of Run().
    SongPlan plan = PlanTune(rom, info, header, settings.loops);
    const uint16_t silenced = SilencedChannels(info, song, settings.version == kUnderWater);
    const Simulation sim = Simulate(rom, info, song, silenced, header, plan, settings.frame_timing, true);
    if (sim.cut_off)
    {
        plan.end = std::min(plan.end, *sim.cut_off);
    }

    SongReport report;
    Summarize(sim, plan, header.ticks_per_quarter, report);
    if (settings.frame_timing)
    {
        plan.loop_start = PlayingFrame(sim.actions, header.ticks_per_quarter, sim.frame_starts, plan.loop_start);
        plan.loop_end = PlayingFrame(sim.actions, header.ticks_per_quarter, sim.frame_starts, plan.loop_end);
        plan.end = OnFrame(sim.frame_starts, plan.end);
    }

    tune.valid = true;
    tune.warnings = sim.warnings;
    tune.truncated = sim.cut_off.has_value();
    tune.ticks_per_quarter = header.ticks_per_quarter;
    tune.loops = plan.loops;
    tune.loop_start = plan.loop_start;
    tune.loop_end = plan.loop_end;
    tune.end = plan.end;
    tune.seconds = report.seconds;
    tune.tempos = TempoChanges(sim.actions);
    tune.stretches = sim.stretches;
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kVolume && a.tick < plan.end)
        {
            tune.volumes.push_back({a.track, a.tick, a.channel, a.a});
        }
    }

    for (const Note& n : NoteCollector(sim, plan, settings.track_mask, kChannels * info.slots_per_channel).Collect())
    {
        tune.notes.push_back({n.track, n.command, n.on, n.off, n.key, n.velocity, n.program});
    }

    return tune;
}

std::vector<SongVersion> TuneVersions(const DriverInfo& info, int song)
{
    if (song != info.water.tune)
    {
        return {SongVersion()};
    }

    return {SongVersion(), {"under water", "_underwater"}};
}

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, 0, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared, int bank)
{
    return Run(rom, info, song, settings, shared, bank, true);
}

SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    SongFiles files;
    files.report = Run(rom, info, song, settings, nullptr, 0, true, &files);

    return files;
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    TuneHeader h;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, song), h))
    {
        error = "invalid tune";
        return false;
    }

    std::string text;
    char line[256];
    std::snprintf(line, sizeof line,
                  "; %s (%s) tune %d\n; header 0x%08X: %u tracks, %u ticks a quarter note, program map 0x%08X, "
                  "instruments 0x%08X\n; columns: address, tick, bytes, command\n",
                  rom.Title().c_str(), rom.GameCode().c_str(), song, unsigned(h.address), unsigned(h.track_count),
                  unsigned(h.ticks_per_quarter), unsigned(h.program_map), unsigned(h.instruments));
    text += line;

    // Each track's commands, up to its end or the end of its first loop.
    for (uint32_t t = 0; t < h.track_count; t++)
    {
        std::snprintf(line, sizeof line, "\n; ---- track %u at 0x%08X ----\n", unsigned(t), unsigned(h.tracks[t]));
        text += line;

        uint32_t address = h.tracks[t];
        uint64_t tick = 0;
        bool loop_started = false;
        for (int n = 0; n < kMaxListedCommands; n++)
        {
            Event e;
            if (!DecodeEvent(rom, address, info.format, e))
            {
                std::snprintf(line, sizeof line, "0x%08X %8llu  %02X  (unknown command; the track stops here)\n",
                              unsigned(address), static_cast<unsigned long long>(tick), rom.U8(address));
                text += line;
                break;
            }

            const std::string bytes = HexBytes(rom, address, e.size);

            // The driver ignores a loop's end before its start.
            const bool control = e.command == kCmdControl;
            const bool loop_end = control && e.a == kCtrlLoopEnd && loop_started;
            const char* const note =
                control && e.a == kCtrlLoopEnd && !loop_started ? " (no loop to end; ignored)" : "";
            std::snprintf(line, sizeof line, "0x%08X %8llu  %-15s %s%s\n", unsigned(address),
                          static_cast<unsigned long long>(tick), bytes.c_str(), DescribeEvent(e).c_str(), note);
            text += line;

            address += e.size;
            loop_started = loop_started || (control && e.a == kCtrlLoopStart);
            if (IsDelay(e.command))
            {
                tick += e.value;
            }

            if (e.command == kCmdEnd || loop_end)
            {
                break;
            }
        }
    }

    return WriteTextFile(path, text, error);
}

} // namespace agblib::sequence_export::rare
