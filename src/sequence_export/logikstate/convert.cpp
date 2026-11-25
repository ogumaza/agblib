// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/logikstate/convert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/logikstate/driver.h"
#include "agblib/logikstate/mod1_player.h"
#include "agblib/logikstate/mod30_player.h"
#include "agblib/logikstate/player.h"
#include "agblib/sequence_export/agb/module_conversion.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/logikstate/soundfont.h"

namespace agblib::sequence_export::logikstate
{

using agblib::agb::LoopStart;
using agblib::logikstate::Build;
using agblib::logikstate::Channel;
using agblib::logikstate::Format;
using agblib::logikstate::Mod1Player;
using agblib::logikstate::Mod30Channel;
using agblib::logikstate::Mod30Player;
using agblib::logikstate::Mod30Settings;
using agblib::logikstate::Player;
using agblib::logikstate::Song;
using agblib::sequence_export::agb::kModuleMaxSeconds;
using agblib::sequence_export::agb::LoopPassesBend;
using agblib::sequence_export::agb::LoopPassSamples;
using agblib::sequence_export::agb::MissingSongReport;
using agblib::sequence_export::agb::ModuleRecorder;
using agblib::sequence_export::agb::ModuleRow;
using agblib::sequence_export::agb::ModuleSimulation;
using agblib::sequence_export::agb::RunModuleConversion;
using agblib::sequence_export::agb::Sounding;
using agblib::sequence_export::agb::TrimVoices;

namespace
{

// The level of a channel at which a SoundFont sample plays at full scale: two FIFOs play each channel's points times
// its volume over 256. One FIFO plays them at half that level.
constexpr double kFullLevel = 128;
constexpr int kFullFifos = 2;

// The share of that level that a game plays once it has switched the sound off: both FIFOs at half volume.
constexpr double kHalfVolume = 0.5;

// A channel of the mixer after a row or a tick. Both of the mixer's sides play the channel alike.
//
// A note plays from the row that starts it, or from the row or tick at which its volume rises from 0. The mixer of the
// build with short records leaves a channel at volume 0 alone, and the sample waits at its point. In the build with
// long records, the sample moves on in silence. A note then lasts through its rows at volume 0, at level 0.
struct Voice
{
    bool playing = false;
    uint32_t starts = 0; // the channel's count of notes and changes of sample
    PlayedSample sample; // with the point that the channel plays as its offset
    int number = 0;      // the sample's number in the module, from 1
    double rate = 0;     // Hz
    double left = 0;     // where kFullLevel is full scale
    double right = 0;
};

using Simulation = ModuleSimulation<Voice>;

// A song, the build of the driver that plays it, the music's volume that the game plays it at, the FIFOs that play
// the mix, and whether the game plays its FIFOs at half volume. A GBAMOD30 module plays with the game's settings of
// the driver.
struct Play
{
    Song song;
    Build build = Build::kShortRecords;
    int32_t music_volume = 0;
    int fifos = kFullFifos;
    bool half_volume = false;
    Mod30Settings mod30;
};

// Returns the share of the full level at which the FIFOs play.
double FifoScale(const Play& play)
{
    return (play.half_volume ? kHalfVolume : 1.0) * play.fifos / kFullFifos;
}

// Returns a player of the song's format.
template <typename P>
P MakePlayer(const Rom& rom, const Play& play)
{
    if constexpr (std::is_same_v<P, Mod1Player>)
    {
        return P(rom, play.song, play.music_volume);
    }
    else if constexpr (std::is_same_v<P, Mod30Player>)
    {
        Mod30Settings settings = play.mod30;
        settings.music = play.music_volume;
        settings.loop = play.song.loop_flag;
        return P(rom, play.song.module, settings);
    }
    else
    {
        return P(rom, play.song, play.music_volume, play.build);
    }
}

// Returns the channels of a player's module, and the mixer's rate in hertz.
template <typename P>
uint32_t ChannelCount(const P& player)
{
    if constexpr (std::is_same_v<P, Mod30Player>)
    {
        return uint32_t(player.Channels());
    }
    else
    {
        return player.Header().channels;
    }
}

template <typename P>
uint32_t MixerRate(const P& player)
{
    if constexpr (std::is_same_v<P, Mod30Player>)
    {
        return player.Rate();
    }
    else
    {
        return player.Header().rate;
    }
}

// Returns channel `number` of the mixer as the player has it after a row or a tick. `scale` is the FIFOs' share of
// their full volume.
template <typename P>
Voice VoiceOf(const Rom& rom, const P& player, int number, double scale)
{
    const Channel& ch = player.Channels()[size_t(number)];
    Voice v;
    v.starts = ch.starts;
    v.playing = player.Sounding(ch, number);
    if (!v.playing)
    {
        return v;
    }

    // The point that the channel plays. Past its sample's end, the channel plays the loop from its start at the mixer's
    // next sample, or stops there.
    v.sample = {ch.first_point,
                ch.whole_length,
                ch.whole_loop_start,
                ch.whole_loop_length,
                SampleRate(rom, player.Header(), ch.fine_tune),
                0,
                Loops(ch.whole_loop_length)};
    const bool in_loop = ch.points != ch.first_point || ch.length != ch.whole_length;
    const uint32_t point = ch.position >> 12;
    if (point >= ch.length && !Loops(ch.loop_length))
    {
        v.playing = false;
        return v;
    }

    if (point >= ch.length)
    {
        v.sample.offset = LoopOffset(v.sample);
    }
    else
    {
        v.sample.offset = in_loop ? LoopOffset(v.sample) + point : point;
    }

    v.number = ch.sample + 1;
    v.rate = double(ch.step) * player.Header().rate / 4096.0;
    v.left = v.right = std::min(player.MixVolume(ch, number) * scale, kFullLevel);

    // At each turn of a loop, the mixer masks the place with kEndPlaceMask. The mask is 0, and the place's fraction is
    // lost. A turn of L points at the step s then takes ceil(L × 4096 / s) samples, not L × 4096 / s, and a short loop
    // plays flat. A loop over the whole sample plays its first pass that way too. A difference below kModuleTuningCents
    // is left out as at a note's start.
    const bool whole_loop = ch.whole_loop_start == 0 && ch.whole_loop_length == ch.whole_length;
    if (Loops(ch.whole_loop_length) && int32_t(ch.step) > 0 && (in_loop || point >= ch.length || whole_loop))
    {
        static_assert(agblib::logikstate::kEndPlaceMask == 0);
        const uint64_t span = uint64_t(ch.whole_loop_length) << 12;
        const uint64_t samples = LoopPassSamples(span, ch.step, 1);
        const double turn = double(span) / (double(samples) * ch.step);
        if (LoopPassesBend(turn))
        {
            v.rate *= turn;
        }
    }

    // In the GBAMOD1 build, a slide down can bring the step to 0 or below it. The sample then stands still or plays
    // backwards. A SoundFont can't play either. The note plays at level 0 until the step is above 0 again or another
    // sample starts, and keeps its bend.
    if constexpr (std::is_same_v<P, Mod1Player>)
    {
        if (int32_t(ch.step) <= 0)
        {
            v.rate = 0;
            v.left = v.right = 0;
        }
    }

    return v;
}

// Returns music channel `number` of a GBAMOD30 module as the player has it after a row or a tick.
Voice VoiceOf(const Rom&, const Mod30Player& player, int number, double scale)
{
    const Mod30Channel& ch = player.Records()[size_t(number)];
    Voice v;
    v.starts = ch.starts;
    v.playing = player.Sounding(ch, number);
    if (!v.playing)
    {
        return v;
    }

    // The point that the channel plays. Past its sample's end, the channel plays the loop from its start at the mixer's
    // next sample, or stops there.
    v.sample = {ch.first_point,
                ch.whole_length,
                ch.whole_loop_start,
                ch.whole_loop_length,
                uint32_t(std::lround(player.RootRate(ch.sample))),
                0,
                ch.whole_loop_length != 0};
    const uint32_t point = ch.position >> 12;
    if (ch.position >= ch.length && ch.loop_length == 0)
    {
        v.playing = false;
        return v;
    }

    if (ch.position >= ch.length)
    {
        v.sample.offset = LoopOffset(v.sample);
    }
    else
    {
        v.sample.offset = ch.looped ? LoopOffset(v.sample) + point : point;
    }

    v.number = ch.sample + 1;
    v.rate = double(ch.step) * player.Rate() / 4096.0;
    v.left = v.right = std::min(player.MixVolume(ch) * scale, kFullLevel);

    return v;
}

// Runs the model until a row revisits an earlier place in the module with the same speed, tempo and pattern-loop
// state, and the module has played the loop between them `loops` times in all (ModuleRecorder).
//
// A song of the GBAMOD1 build also ends at the first row whose order is past the module's last. The driver then clears
// the channels at each frame, and reads rows from the bytes after the module's orders. A song that plays once ends
// where its first row with a jump starts: the game's jingle routine goes back to the piece before at that jump, and
// clears the channels. Neither has a loop.
template <typename P>
Simulation Simulate(const Rom& rom, const Play& play, int loops, LoopStart start_pass)
{
    Simulation sim;
    P player = MakePlayer<P>(rom, play);
    if (!player.Valid())
    {
        sim.warnings = player.Warnings();
        return sim;
    }

    sim.channels = ChannelCount(player);
    sim.rate = MixerRate(player);

    // Two orders that play on alike count as one place. In a GBAMOD30 module, orders RepeatOrders() apart play on
    // alike.
    uint32_t repeat_orders = UINT32_MAX;
    if constexpr (std::is_same_v<P, Mod30Player>)
    {
        repeat_orders = player.RepeatOrders();
    }

    ModuleRecorder recorder(sim, loops, start_pass);
    const double scale = FifoScale(play);
    const auto record = [&](bool row)
    {
        if (recorder.Full())
        {
            return;
        }

        // A row past the module's last order ends the song.
        if (row && !recorder.Ended() && uint32_t(player.RowOrder()) >= player.Header().order_count)
        {
            recorder.EndAt(sim.rows.size());
        }

        if (row)
        {
            const int order = int(uint32_t(player.RowOrder()) % repeat_orders);
            recorder.Visit({order, player.RowNumber(), player.RepeatState()});
            recorder.AddRow(player.RowOrder(), player.RowNumber(), player.RowSamples());
        }

        if constexpr (std::is_same_v<P, Mod1Player>)
        {
            if (row && !recorder.Ended() && play.song.plays_once && player.RowJumped())
            {
                recorder.EndAt(sim.rows.size() - 1);
            }
        }

        bool sounds = false;
        for (uint32_t c = 0; c < sim.channels; c++)
        {
            sim.voices.push_back(VoiceOf(rom, player, int(c), scale));
            sounds = sounds || Sounding(sim.voices.back());
        }

        recorder.AddStep(player.Elapsed(), sounds);
    };
    player.OnStep(record);

    const uint64_t most = kModuleMaxSeconds * uint64_t(sim.rate);
    while (player.Playing() && recorder.Wants() && player.Elapsed() < most)
    {
        player.Frame();
    }

    sim.warnings = player.Warnings();
    if (sim.rows.empty())
    {
        return sim;
    }

    // A GBAMOD30 module without the loop flag stops after its last order's last row. The mixer plays on what the
    // channels contain, and the song ends where that row ends.
    if constexpr (std::is_same_v<P, Mod30Player>)
    {
        if (!recorder.Ended() && !player.Playing() && player.Valid())
        {
            const ModuleRow last = sim.rows.back();
            const uint64_t time = sim.steps[last.step].time + last.samples;
            recorder.AddRow(last.order, last.pattern_row + 1, last.samples);
            sim.voices.resize(sim.voices.size() + sim.channels);
            recorder.AddStep(time, false);
        }
    }

    recorder.Finish(player.Playing(), "found no loop within an hour, so the module is cut off there");
    TrimVoices(sim);

    return sim;
}

// Converts a song as Run() does with the player of the song's format.
template <typename P>
SongReport RunWith(const Rom& rom, const Play& play, int song, int count, const std::string& source,
                   const ConvertSettings& settings, InstrumentSet* shared, bool write, SongFiles* files)
{
    const auto simulate = [&](int loops, LoopStart start_pass)
    {
        return Simulate<P>(rom, play, loops, start_pass);
    };

    return RunModuleConversion(rom, song, count, source, kRootKey, simulate, settings, shared, write, files);
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

    const Song& entry = info.songs[size_t(song)];
    const Play play = {entry, info.build, info.music_volume, info.fifos, info.sound_off_call != 0, info.mod30};
    const int count = int(info.songs.size());
    char source[64];
    std::snprintf(source, sizeof source, "module at 0x%08X from order %d", unsigned(entry.module), entry.order);
    switch (info.format)
    {
    case Format::kGbaMod1:
        return RunWith<Mod1Player>(rom, play, song, count, source, settings, shared, write, files);

    case Format::kGbaMod30:
        return RunWith<Mod30Player>(rom, play, song, count, source, settings, shared, write, files);

    case Format::kGbaMod2:
        break;
    }

    return RunWith<Player>(rom, play, song, count, source, settings, shared, write, files);
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

} // namespace agblib::sequence_export::logikstate
