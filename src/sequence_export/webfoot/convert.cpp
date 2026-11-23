// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/webfoot/convert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/webfoot/soundfont.h"
#include "agblib/webfoot/driver.h"
#include "agblib/webfoot/player.h"

namespace agblib::sequence_export::webfoot
{

using agblib::agb::LoopStart;
using agblib::sequence_export::agb::kModuleMaxSeconds;
using agblib::sequence_export::agb::ModuleRecorder;
using agblib::sequence_export::agb::ModuleSimulation;
using agblib::sequence_export::agb::RunModuleConversion;
using agblib::sequence_export::agb::Sounding;
using agblib::sequence_export::agb::TrimVoices;
using agblib::webfoot::kChannels;
using agblib::webfoot::kFractionBits;
using agblib::webfoot::kSampleSize;
using agblib::webfoot::kVoiceOn;
using agblib::webfoot::Mixer;
using agblib::webfoot::Player;
using agblib::webfoot::ReadSample;
using agblib::webfoot::Sample;
using agblib::webfoot::SongHeader;
using agblib::webfoot::SongTable;
using agblib::webfoot::Voice;

namespace
{

// A side's level at which a SoundFont sample plays at full scale: a FIFO plays a point times the side's level over 64.
// The MIDI writer's levels have full scale at 128.
constexpr double kFullSideLevel = 64;
constexpr double kFullLevel = 128;

// A voice after a tick.
struct VoiceState
{
    bool playing = false;
    uint32_t starts = 0; // the voice's count of the starts of its sample
    PlayedSample sample; // with the point that the voice plays as its offset
    int number = 0;      // the sample's number in its table, from 0
    double rate = 0;     // Hz
    double left = 0;     // where kFullLevel is full scale
    double right = 0;
};

using Simulation = ModuleSimulation<VoiceState>;

// Returns voice `c` as the player has it after a tick.
VoiceState VoiceOf(const Rom& rom, const Player& player, int c)
{
    const Voice& v = player.Voices()[size_t(c)];
    VoiceState s;
    s.starts = v.starts;
    s.playing = (v.flags & kVoiceOn) != 0 && v.speed > 0;
    if (!s.playing)
    {
        return s;
    }

    // A note that starts past its sample's end plays nothing.
    const Mixer& mixer = player.MixerRules();
    const Sample entry = ReadSample(rom, v.sample);
    s.sample.offset = uint32_t(std::max(v.position, 0) >> kFractionBits);
    if (s.sample.offset >= entry.length)
    {
        s.playing = false;
        return s;
    }

    s.sample.entry = v.sample;
    s.sample.points = entry.points;
    s.sample.length = entry.length;
    s.sample.loop_start = entry.loop_start;
    s.sample.flags = entry.flags;
    s.sample.rate = uint32_t(std::lround(PlayedRate(entry.rate, mixer)));
    s.number = int((v.sample - player.Header().samples) / kSampleSize);
    s.rate = double(v.speed) * mixer.Rate() / double(1u << kFractionBits);
    s.left = std::min(v.left * kFullLevel / kFullSideLevel, kFullLevel);
    s.right = std::min(v.right * kFullLevel / kFullSideLevel, kFullLevel);

    return s;
}

// Runs the model until a row revisits an earlier place in the song with the same speed and tempo, and the song has
// played the loop between them `loops` times in all (ModuleRecorder). The model mixes the runs of the game's mix
// routine one at a time as the game's interrupt handler starts them. A voice's place in a ping-pong loop depends on
// where each run starts.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, LoopStart start_pass)
{
    Simulation sim;
    int index = 0;
    const SongTable* table = info.TableOf(song, index);
    if (table == nullptr)
    {
        sim.warnings.push_back("there's no song " + std::to_string(song));
        return sim;
    }

    Player player(rom, info, song, table->music_volume);
    if (!player.Valid())
    {
        sim.warnings = player.Warnings();
        return sim;
    }

    sim.channels = kChannels;
    sim.rate = player.MixerRules().Rate();

    ModuleRecorder recorder(sim, loops, start_pass);
    const auto record = [&](bool row)
    {
        if (recorder.Full())
        {
            return;
        }

        if (row)
        {
            recorder.Visit({player.RowOrder(), player.RowNumber(), player.RepeatState()});
            recorder.AddRow(player.RowOrder(), player.RowNumber(), player.RowSamples());
        }

        if (sim.rows.empty())
        {
            return;
        }

        bool sounds = false;
        for (int c = 0; c < kChannels; c++)
        {
            sim.voices.push_back(VoiceOf(rom, player, c));
            sounds = sounds || Sounding(sim.voices.back());
        }

        recorder.AddStep(player.Elapsed(), sounds);
    };
    player.OnStep(record);

    const uint64_t most = kModuleMaxSeconds * uint64_t(sim.rate);
    while (player.Playing() && recorder.Wants() && player.Elapsed() < most)
    {
        player.Mix(table->mix_samples, nullptr);
    }

    sim.warnings = player.Warnings();
    if (sim.rows.empty())
    {
        return sim;
    }

    recorder.Finish(player.Playing(), "found no loop within an hour, so the song is cut off there");
    TrimVoices(sim);

    // A row lasts until the next starts. A row at speed 0 lasts 256 ticks. Its length from the player leaves those
    // ticks out.
    for (size_t r = 0; r + 1 < sim.rows.size(); r++)
    {
        sim.rows[r].samples = uint32_t(sim.steps[sim.rows[r + 1].step].time - sim.steps[sim.rows[r].step].time);
    }

    return sim;
}

// Converts a song, writing its files if `write` is set. With `files`, the files' bytes go there instead of to files,
// and the report names no paths.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write, SongFiles* files = nullptr)
{
    int index = 0;
    const SongTable* table = info.TableOf(song, index);
    if (table == nullptr)
    {
        SongReport report;
        report.warnings.push_back("there's no song " + std::to_string(song));
        return report;
    }

    const SongHeader& header = table->songs[size_t(index)];
    const auto simulate = [&](int loops, LoopStart start_pass)
    {
        return Simulate(rom, info, song, loops, start_pass);
    };

    char source[64];
    std::snprintf(source, sizeof source, "song at 0x%08X", unsigned(header.address));
    SongReport report =
        RunModuleConversion(rom, song, info.SongCount(), source, kRootKey, simulate, settings, shared, write, files);
    report.address = header.address;

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

} // namespace agblib::sequence_export::webfoot
