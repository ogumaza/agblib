// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/frontline_xm/convert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/frontline_xm/driver.h"
#include "agblib/frontline_xm/player.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/frontline_xm/soundfont.h"

namespace agblib::sequence_export::frontline_xm
{

using agblib::agb::LoopStart;
using agblib::frontline_xm::Channel;
using agblib::frontline_xm::kChannels;
using agblib::frontline_xm::kInstrumentFinetune;
using agblib::frontline_xm::kInstrumentLength;
using agblib::frontline_xm::kInstrumentLoopEnd;
using agblib::frontline_xm::kInstrumentLoopStart;
using agblib::frontline_xm::kInstrumentLoopType;
using agblib::frontline_xm::kInstrumentPoints;
using agblib::frontline_xm::kPositionShift;
using agblib::frontline_xm::Module;
using agblib::frontline_xm::Player;
using agblib::frontline_xm::Song;
using agblib::sequence_export::agb::FinishModuleSimulation;
using agblib::sequence_export::agb::kModuleMaxSeconds;
using agblib::sequence_export::agb::ModuleRecorder;
using agblib::sequence_export::agb::ModuleSimulation;
using agblib::sequence_export::agb::RunModuleSong;
using agblib::sequence_export::agb::Sounding;

namespace
{

// The MIDI writer's levels have full scale at 128.
constexpr double kFullLevel = 128;

// A point at the gain 2^14 (volume x master volume) plays at full scale.
constexpr double kFullGain = 1 << 14;

// A position's unit, and the whole points of a position, rounded up.
constexpr uint32_t kPointUnit = 1u << kPositionShift;

uint32_t WholePoints(uint32_t position)
{
    return uint32_t((uint64_t(position) + kPointUnit - 1) >> kPositionShift);
}

// A channel after a tick.
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

// Returns channel `c` as the player leaves it after a tick, before the mix of the samples after it.
VoiceState VoiceOf(const Rom& rom, const DriverInfo& info, uint8_t master, const Channel& c)
{
    VoiceState s;
    s.starts = c.starts;
    s.playing = c.on != 0 && c.instrument != 0 && c.step > 0;
    if (!s.playing)
    {
        return s;
    }

    const uint32_t record = c.instrument;
    const uint32_t length = rom.U32(record + kInstrumentLength);
    const uint32_t loop_start = rom.U32(record + kInstrumentLoopStart);
    const uint32_t loop_end = rom.U32(record + kInstrumentLoopEnd);
    s.sample.points = record + kInstrumentPoints;
    s.sample.length = WholePoints(length);
    s.sample.loops = rom.U8(record + kInstrumentLoopType) != 0 && loop_end > loop_start;
    if (s.sample.loops)
    {
        s.sample.loop_start = loop_start >> kPositionShift;
        s.sample.loop_end = WholePoints(loop_end);
    }
    s.sample.rate = uint32_t(std::lround(RootRate(rom, info, rom.S8(record + kInstrumentFinetune))));

    // The point that the channel plays next. Past its end, the mixer goes to the loop or stops the channel.
    uint32_t position = c.position;
    if (position >= c.end)
    {
        if (!s.sample.loops)
        {
            s.playing = false;
            return s;
        }
        position = loop_start + (position - loop_end);
    }
    s.sample.offset = position >> kPositionShift;

    s.number = c.number;
    s.rate = double(c.step) * info.Rate(rom) / kPointUnit;
    s.left = std::min(double(c.volume) * master * kFullLevel / kFullGain, kFullLevel);
    s.right = s.left;

    return s;
}

// Runs the model until a row comes back to an earlier row's place in the module with the same speed and BPM, and the
// song has played the loop between them `loops` times in all (ModuleRecorder). Each tick is a step, read after the
// tick and before the mix of the samples after it. The ticks before the first row are left out: the channels are
// silent.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, LoopStart start_pass)
{
    Simulation sim;
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        sim.warnings.push_back("there's no song " + std::to_string(song));
        return sim;
    }

    Player player(rom, info);
    player.Start(song);
    sim.channels = kChannels;
    sim.rate = info.Rate(rom);

    ModuleRecorder recorder(sim, loops, start_pass);
    player.on_tick = [&]()
    {
        if (recorder.Full())
        {
            return;
        }
        if (player.RowRead())
        {
            const std::vector<uint32_t> state = {player.Speed(), player.Bpm()};
            recorder.Visit({player.RowOrder(), player.RowNumber(), state});
            recorder.AddRow(player.RowOrder(), player.RowNumber(),
                            std::max<uint32_t>(player.Speed(), 1) * player.TickSamples());
        }
        if (sim.rows.empty())
        {
            return;
        }

        bool sounds = false;
        for (int c = 0; c < kChannels; c++)
        {
            sim.voices.push_back(VoiceOf(rom, info, player.Master(), player.ChannelAt(c)));
            sounds = sounds || Sounding(sim.voices.back());
        }
        recorder.AddStep(player.Elapsed(), sounds);
    };

    const uint64_t most = kModuleMaxSeconds * uint64_t(sim.rate);
    while (recorder.Wants() && player.Elapsed() < most)
    {
        player.Frame();
    }

    FinishModuleSimulation(sim, recorder, true, uint64_t(player.Unmodelled()));

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
        std::snprintf(source, sizeof source, "module at 0x%08X, list at 0x%08X", unsigned(module.address),
                      unsigned(entry.list));
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

} // namespace agblib::sequence_export::frontline_xm
