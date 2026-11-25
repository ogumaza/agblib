// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/frontline_mod/convert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/frontline_mod/driver.h"
#include "agblib/frontline_mod/player.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/frontline_mod/soundfont.h"

namespace agblib::sequence_export::frontline_mod
{

using agblib::agb::LoopStart;
using agblib::frontline_mod::Channel;
using agblib::frontline_mod::kFullVolume;
using agblib::frontline_mod::kInstrumentLoopEnd;
using agblib::frontline_mod::kInstrumentLoopStart;
using agblib::frontline_mod::kInstrumentSample;
using agblib::frontline_mod::kSampleLength;
using agblib::frontline_mod::kSamplePoints;
using agblib::frontline_mod::Module;
using agblib::frontline_mod::Player;
using agblib::frontline_mod::Song;
using agblib::sequence_export::agb::FinishModuleSimulation;
using agblib::sequence_export::agb::kModuleMaxSeconds;
using agblib::sequence_export::agb::LoopPassesBend;
using agblib::sequence_export::agb::LoopPassSamples;
using agblib::sequence_export::agb::MissingSongWarning;
using agblib::sequence_export::agb::ModuleRecorder;
using agblib::sequence_export::agb::ModuleSimulation;
using agblib::sequence_export::agb::RunModuleSong;
using agblib::sequence_export::agb::Sounding;

namespace
{

// The MIDI writer's levels have full scale at 128.
constexpr double kFullLevel = 128;

// The fraction bits of a step.
constexpr double kStepUnit = 65536;

// The clip table turns the sums of its span into 256 bytes.
constexpr double kClipBytes = 256;

// A channel's gain scales a point by the gain over 64.
constexpr double kGainUnit = 64;

// A channel after a frame's tick or row.
struct VoiceState
{
    bool playing = false;
    uint32_t starts = 0; // the channel's count of the starts of its sample
    PlayedSample sample; // with the point that the channel plays next as its offset
    int number = 0;      // the instrument's number in its module
    double rate = 0;     // Hz
    double left = 0;     // where kFullLevel is full scale
    double right = 0;
};

using Simulation = ModuleSimulation<VoiceState>;

// Returns the rate in hertz at which the mixer plays a loop of `span` + 1 points with the 16.16 step `step`, at the
// mixer's rate `rate`. The mixer checks the loop's end once every two samples and restarts the loop with the fraction
// at 0. Each pass therefore lasts N samples, the first even count whose points pass the loop's end: N x step >= (span +
// 1) x 65536.
double LoopRate(uint32_t step, uint32_t span, double rate)
{
    const uint64_t samples = LoopPassSamples((uint64_t(span) + 1) << 16, step, 2);
    return double(span + 1) * rate / double(samples);
}

// Returns the music's volume and the fade step as the player's next row leaves them, where a step that only holds the
// volume at an end counts as no step. Two rows whose fades give the same values play the same levels from there on.
std::vector<uint32_t> FadeState(const Player& player)
{
    const int32_t step = player.FadeStep();
    const int32_t volume = std::clamp(player.MusicVolume() + step, 0, kFullVolume);
    const bool held = (volume == kFullVolume && step > 0) || (volume == 0 && step < 0);
    return {uint32_t(volume), held ? 0u : uint32_t(step)};
}

// Returns channel `c` as the player leaves it before the frame's mix.
VoiceState VoiceOf(const Rom& rom, const DriverInfo& info, const Module& module, const Channel& c, double root_rate)
{
    VoiceState s;
    s.starts = c.starts;
    s.playing = c.on != 0 && c.step > 0;
    if (!s.playing)
    {
        return s;
    }

    const uint32_t record = rom.U32(module.instruments + 4 * c.instrument);
    const uint32_t sample = rom.U32(record + kInstrumentSample);
    s.sample.points = rom.U32(sample + kSamplePoints);
    s.sample.length = rom.U32(sample + kSampleLength);
    s.sample.loop_start = rom.U32(record + kInstrumentLoopStart);
    s.sample.loop_end = rom.U32(record + kInstrumentLoopEnd);
    s.sample.rate = uint32_t(std::lround(root_rate));

    // A note that starts past its loop's end plays from the loop's start. One that starts past the end of a sample
    // without a loop plays nothing.
    s.sample.offset = c.position - s.sample.points;
    const bool past_loop = Loops(s.sample) && s.sample.offset > s.sample.loop_end;
    if (past_loop)
    {
        s.sample.offset = s.sample.loop_start;
    }
    if (!Loops(s.sample) && s.sample.offset >= s.sample.length)
    {
        s.playing = false;
        return s;
    }

    // The clip table maps its span of sums to 256 bytes: a point at the full gain plays at full scale.
    const double full_gain = kGainUnit * info.mixer_settings.ClipSpan() / kClipBytes;
    const uint32_t gain = (c.level * info.mixer_settings.master) >> 6;
    s.number = int(c.instrument);
    s.rate = double(c.step) * info.Rate() / kStepUnit;

    // From the mixer's first restart of the loop, the note plays at the rate of the loop's passes: a bend from the
    // step's pitch. A difference below kModuleTuningCents is left out, as at a note's start.
    if (past_loop || (Loops(s.sample) && c.wraps > 0))
    {
        const double loop_rate = LoopRate(c.step, s.sample.loop_end - s.sample.loop_start, info.Rate());
        if (LoopPassesBend(loop_rate / s.rate))
        {
            s.rate = loop_rate;
        }
    }

    s.left = std::min(gain * kFullLevel / full_gain, kFullLevel);
    s.right = s.left;

    return s;
}

// Runs the model until a row comes back to an earlier row's place in the module with the same speed and fade, and the
// song has played the loop between them `loops` times in all (ModuleRecorder). Each frame is a step: the player's tick
// or row, read before the frame's mix.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, LoopStart start_pass)
{
    Simulation sim;
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        sim.warnings.push_back(MissingSongWarning(song));
        return sim;
    }

    const Song& entry = info.songs[size_t(song)];
    const Module& module = info.modules[size_t(entry.module)];
    Player player(rom, info);
    player.Start(module, entry.order, entry.step);
    sim.channels = player.MusicChannels();
    sim.rate = info.Rate();
    const double root_rate = RootRate(rom, info);
    const uint32_t samples = info.mixer_settings.samples;

    ModuleRecorder recorder(sim, loops, start_pass);
    const uint64_t most = kModuleMaxSeconds * uint64_t(sim.rate);
    uint64_t time = 0;
    while (player.Playing() && recorder.Wants() && time < most)
    {
        const int order = int(player.Order());
        const int row = int(player.Row());
        std::vector<uint32_t> state = FadeState(player);
        const bool read = player.Play();
        if (!recorder.Full())
        {
            // A row's ticks and the rows after it play at the speed that the row leaves.
            if (read)
            {
                state.push_back(player.Speed());
                recorder.Visit({order, row, state});
                recorder.AddRow(order, row, std::max<uint32_t>(player.Speed(), 1) * samples);
            }
            if (!sim.rows.empty())
            {
                bool sounds = false;
                for (uint32_t c = 0; c < sim.channels; c++)
                {
                    sim.voices.push_back(VoiceOf(rom, info, module, player.ChannelAt(c), root_rate));
                    sounds = sounds || Sounding(sim.voices.back());
                }
                recorder.AddStep(time, sounds);
            }
        }

        player.Mix();
        time += samples;
    }

    FinishModuleSimulation(sim, recorder, player.Playing(), uint64_t(player.Unmodelled()));

    return sim;
}

// Converts a song, writing its files if `write` is set. With `files`, the files' bytes go there instead of to files,
// and the report names no paths.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write, SongFiles* files = nullptr)
{
    const auto simulate = [&](int loops, LoopStart start_pass)
    {
        return Simulate(rom, info, song, loops, start_pass);
    };
    const auto describe = [](const Song& entry, const Module& module)
    {
        char source[64];
        std::snprintf(source, sizeof source, "module at 0x%08X, order %u", unsigned(module.address),
                      unsigned(entry.order));
        return std::string(source);
    };
    return RunModuleSong(rom, info, song, describe, kRootKey, simulate, settings, shared, write, files);
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

} // namespace agblib::sequence_export::frontline_mod
