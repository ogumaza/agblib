// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/engine_software/convert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/engine_software/driver.h"
#include "agblib/engine_software/player.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/engine_software/soundfont.h"

namespace agblib::sequence_export::engine_software
{

using agblib::agb::LoopStart;
using agblib::engine_software::Channel;
using agblib::engine_software::kChannelOff;
using agblib::engine_software::kChannelPaused;
using agblib::engine_software::kFractionBits;
using agblib::engine_software::kInstrumentSamples;
using agblib::engine_software::Player;
using agblib::sequence_export::agb::FinishModuleSimulation;
using agblib::sequence_export::agb::kModuleMaxSeconds;
using agblib::sequence_export::agb::MissingSongReport;
using agblib::sequence_export::agb::ModuleRecorder;
using agblib::sequence_export::agb::ModuleSimulation;
using agblib::sequence_export::agb::RunModuleConversion;
using agblib::sequence_export::agb::Sounding;

namespace
{

// The MIDI writer's levels have full scale at 128.
constexpr double kFullLevel = 128;

// A level of a quarter of the clip table's slope plays a sample's points at full scale.
constexpr double kFullScaleShare = 4;

// A channel after a tick.
struct VoiceState
{
    bool playing = false;
    uint32_t starts = 0; // the channel's count of the starts of its sample
    PlayedSample sample; // with the point that the channel plays next as its offset
    int number = 0;      // the instrument's number in the bank
    double rate = 0;     // Hz
    double left = 0;     // where kFullLevel is full scale
    double right = 0;
};

using Simulation = ModuleSimulation<VoiceState>;

// Returns the instrument's number in the bank, from 1, of the record at `record`.
int InstrumentNumber(const DriverInfo& info, uint32_t record)
{
    const auto& records = info.bank.instruments;
    return int(std::find(records.begin(), records.end(), record) - records.begin()) + 1;
}

// Returns channel `c` as the player leaves it after a tick, before the mix of the samples after it.
VoiceState VoiceOf(const Rom& rom, const DriverInfo& info, const Player& player, const Channel& c)
{
    VoiceState s;
    s.starts = c.starts;
    s.playing = !(c.flags & (kChannelOff | kChannelPaused)) && c.sample_instrument != 0 && c.step > 0 &&
                c.end > c.sample && c.place < c.end;
    if (!s.playing)
    {
        return s;
    }

    // The mixer plays the points from the sample's start to the channel's end. A loop takes the loop's bytes before the
    // end.
    const uint32_t record = c.sample_instrument;
    s.sample.points = record + kInstrumentSamples;
    s.sample.length = c.end - s.sample.points;
    s.sample.loops = c.loop_bytes != 0 && c.loop_bytes <= s.sample.length;
    if (s.sample.loops)
    {
        s.sample.loop_start = s.sample.length - c.loop_bytes;
    }

    s.sample.rate = uint32_t(std::lround(RootRate(rom, info, record)));
    s.sample.offset = c.place - s.sample.points;
    s.number = InstrumentNumber(info, record);
    s.rate = double(c.step) * info.OutputRate() / double(1u << kFractionBits);

    // Mono: both sides at the channel's level.
    s.left = std::min(double(c.level) * kFullScaleShare * kFullLevel / double(player.ClipSpan()), kFullLevel);
    s.right = s.left;

    return s;
}

// Runs the model until a row comes back to an earlier row's place in the song with the same speed and tempo, and the
// song has played the loop between them `loops` times in all (ModuleRecorder), or until a jingle ends. The driver takes
// the next order as it reads the last row of a pattern. Past a jingle's last order, it stops the song at the end of
// that frame. The jingle's last row is therefore the row at which the song ends. A song that the game plays for one
// pass ends at the first row after the driver's return to the restart order. Each tick is a step, read after the tick
// and before the mix of the samples after it. The ticks before the first row are left out: the channels are silent.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, LoopStart start_pass)
{
    Simulation sim;
    Player player(rom, info);
    player.Start(song);
    sim.channels = uint32_t(player.MusicChannels());
    sim.rate = info.OutputRate();

    // The driver counts the pass as it reads the first pass's last row. The count is therefore read a row late.
    const bool one_pass = info.songs[size_t(song)].start.one_pass;
    uint8_t passes = 0;

    ModuleRecorder recorder(sim, loops, start_pass);
    player.on_tick = [&]()
    {
        if (recorder.Full())
        {
            return;
        }
        if (player.RowRead())
        {
            if (one_pass && passes > 0 && !recorder.Ended())
            {
                recorder.EndAt(sim.rows.size());
            }
            passes = player.Passes();

            const std::vector<uint32_t> state = {player.Speed(), player.Tempo()};
            if (player.Playing())
            {
                recorder.Visit({player.RowOrder(), player.RowNumber(), state});
            }
            recorder.AddRow(player.RowOrder(), player.RowNumber(),
                            std::max<uint32_t>(player.Speed(), 1) * player.TickSamples());
        }
        if (sim.rows.empty())
        {
            return;
        }

        bool sounds = false;
        for (int c = 0; c < player.MusicChannels(); c++)
        {
            sim.voices.push_back(VoiceOf(rom, info, player, player.ChannelAt(c)));
            sounds = sounds || Sounding(sim.voices.back());
        }
        recorder.AddStep(player.Elapsed(), sounds);
    };

    const uint64_t most = kModuleMaxSeconds * uint64_t(sim.rate);
    while (recorder.Wants() && player.Playing() && player.Elapsed() < most)
    {
        player.Frame();
    }

    FinishModuleSimulation(sim, recorder, player.Playing(), uint64_t(player.Unmodelled()));

    return sim;
}

// Converts a song, writing its files if `write` is set. With `files`, the files' bytes go there instead of to files,
// and the report names no paths.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write, SongFiles* files = nullptr)
{
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        return MissingSongReport(song);
    }

    const auto simulate = [&](int loops, LoopStart start_pass)
    {
        return Simulate(rom, info, song, loops, start_pass);
    };
    const uint32_t address = info.songs[size_t(song)].address;
    char source[64];
    std::snprintf(source, sizeof source, "song at 0x%08X, bank at 0x%08X", unsigned(address),
                  unsigned(info.bank.address));
    SongReport report = RunModuleConversion(rom, song, int(info.songs.size()), source, kRootKey, simulate, settings,
                                            shared, write, files);
    report.address = address;

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

} // namespace agblib::sequence_export::engine_software
