// SPDX-License-Identifier: MIT

// Unit tests of the MIDI files of Krawall's conversion: bars, time signatures, loops and the sharing of MIDI
// channels. They build small synthetic cartridges in memory. No game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/krawall/archive_player.h"
#include "agblib/krawall/commands.h"
#include "agblib/krawall/driver.h"
#include "agblib/krawall/instruments.h"
#include "agblib/krawall/player.h"
#include "agblib/krawall/song.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/krawall/convert.h"
#include "agblib/sequence_export/krawall/performance.h"
#include "agblib/sequence_export/krawall/soundfont.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/krawall_cart.h"
#include "krawall_test_util.h"

namespace agblib::sequence_export::krawall
{
namespace
{

// Returns a MIDI file's time signatures, as the row each starts on, its numerator and its denominator's power of 2.
std::vector<std::vector<uint32_t>> TimeSignatures(const std::string& path)
{
    std::vector<std::vector<uint32_t>> signatures;
    for (const auto& [tick, bytes] : ReadMidi(path).Find(0xFF))
    {
        if (bytes[1] == 0x58)
        {
            signatures.push_back({uint32_t(tick / uint32_t(kSpeed)), bytes[3], bytes[4]});
        }
    }

    return signatures;
}

// Bars follow pattern boundaries, with a short bar when an order ends partway through one. At eight rows per quarter
// note, a 4/4 bar has 32 rows. A break ends the first order after 33 rows. Each pass therefore starts with a one-row
// pickup.
// The 48-row patterns use 12/8: two bars per pattern, at two rows per eighth note.
void TestBars()
{
    Cart cart;
    cart.Module(1, {{64, {{1, 0, 49, 1}, {32, 0, -1, 0, -1, kBreak, 0x00}}},
                    {64, {{0, 0, 49, 1}, {32, 0, 49, 1}}},
                    {40, {{0, 0, 49, 1}, {32, 0, 49, 1}}}});
    cart.Module(1, 64, {{0, 0, 49, 1}});
    cart.Module(1, {{48, {{0, 0, 49, 1}}}, {48, {{0, 0, 49, 1}}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_bars";

    const SongReport cut = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport full = ConvertSong(rom, info, 1, opt, nullptr);
    const SongReport compound = ConvertSong(rom, info, 2, opt, nullptr);

    // Each pass starts with a one-row pickup, followed by full bars and a one-quarter-note bar at the end of the third
    // pattern. The unbroken 64-row pattern stays in 4/4. The 48-row patterns use 12/8, with four rows per quarter note.
    const std::vector<std::vector<uint32_t>> kPasses = {{0, 1, 5},   {1, 4, 2},   {129, 1, 2},
                                                        {137, 1, 5}, {138, 4, 2}, {266, 1, 2}};
    SEQLIB_CHECK(cut.ok && full.ok && compound.ok);
    SEQLIB_CHECK(TimeSignatures(cut.midi_path) == kPasses);
    SEQLIB_CHECK(TimeSignatures(full.midi_path) == std::vector<std::vector<uint32_t>>({{0, 4, 2}}));
    SEQLIB_CHECK(TimeSignatures(compound.midi_path) == std::vector<std::vector<uint32_t>>({{0, 12, 3}}));
    SEQLIB_CHECK_EQ(ReadMidi(compound.midi_path).division, 4 * kSpeed);
}

// The intro's row pans the channel to the left, and the loop plays a note on its first row and then pans the channel to
// the right. The channel keeps that pan when the module jumps back. The first pass's note plays on the left and the
// later passes' on the right. The marked loop therefore starts at the second pass, and the module plays the first pass
// once before it.
void TestLoopStartsAtSecondPass()
{
    Cart cart;
    cart.Module(1, {{1, {{0, 0, -1, 0, -1, kPan, 0x00}}},
                    {4, {{0, 0, 49, 1, -1, -1, 0}, {2, 0, -1, 0, -1, kPan, 0x80}, {3, 0, -1, 0, -1, kJump, 0x01}}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_second";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    // The loop runs from the second pass, after the intro's row and the first pass's four, for four rows. The file
    // plays the note in the first pass and in the loop's two passes.
    const double kRowSeconds = kSpeed * double(kTickSamples) / 16384;
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(std::fabs(sum.loop_start - 5 * kRowSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 9 * kRowSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 13 * kRowSeconds) < 1e-9);
    SEQLIB_REQUIRE_EQ(ReadMidi(sum.midi_path).Find(0x90).size(), 3u);
}

// A module can have up to 32 channels, and --tracks can name each of them. In a module with more than 15, channels
// share MIDI channels, and channels whose notes sound at once get MIDI channels of their own.
void TestManyChannels()
{
    Cart cart;
    cart.Module(18, 4, {{0, 0, 49, 1}, {0, 17, 53, 2}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_channels";
    ConvertSettings first_only = opt;
    first_only.track_mask = 1;
    ConvertSettings last_only = opt;
    last_only.track_mask = 1u << 17;
    ConvertSettings first_sixteen = opt;
    first_sixteen.track_mask = 0xFFFF;

    const SongReport every = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport chosen = InspectSong(rom, info, 0, first_only);
    const SongReport last = InspectSong(rom, info, 0, last_only);
    const SongReport sixteen = InspectSong(rom, info, 0, first_sixteen);

    SEQLIB_CHECK(every.ok && chosen.ok && last.ok && sixteen.ok);
    SEQLIB_CHECK_EQ(every.tracks, 2);
    SEQLIB_CHECK_EQ(chosen.tracks, 1);
    SEQLIB_CHECK_EQ(last.tracks, 1);
    SEQLIB_CHECK_EQ(sixteen.tracks, 1);

    // The 18th channel's E-4 sounds at once with the first channel's note. The 18th channel therefore takes a MIDI
    // channel of its own: channel 16, after those of the first 14 channels. Its note is on key 64 in each of the
    // loop's two passes.
    const auto notes = ReadMidi(every.midi_path).Find(0x9F);
    SEQLIB_CHECK(notes.size() == 2 && notes[0].bytes[1] == 64 && notes[1].bytes[1] == 64);
    SEQLIB_CHECK_EQ(ReadMidi(every.midi_path).Find(0x90).size(), 2u);
}

// Two channels whose notes never sound at once share a MIDI channel. A MIDI channel's program and controllers act on
// all its notes. Each note of a shared MIDI channel therefore sets them again, and the tracks don't set them at the
// start.
void TestSharedMidiChannel()
{
    // 14 channels hold looping notes. Channels 15 and 16 play the sample that doesn't loop, an octave up, at rows 0 and
    // 3. Each note ends before the other starts.
    std::vector<Cell> cells;
    for (int c = 0; c < 14; c++)
    {
        cells.push_back({0, c, 49, 1});
    }

    cells.push_back({0, 14, 61, 2});
    cells.push_back({3, 15, 61, 2, -1, kPan, 0x20});
    Cart cart;
    cart.Module(16, 6, cells);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_shared";

    const SongReport report = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(report.ok);
    SEQLIB_CHECK(report.warnings.empty());
    const agblib::testing::agb_files::MidiEvents midi = ReadMidi(report.midi_path);
    const std::vector<std::string> names = seqlib::midi::TrackNames(midi);
    SEQLIB_CHECK_EQ(midi.Find(0x9F).size(), 4u);
    for (const char* name : {"Channel 15", "Channel 16"})
    {
        const size_t t = size_t(std::find(names.begin(), names.end(), name) - names.begin());
        SEQLIB_REQUIRE(t < midi.tracks.size());
        uint64_t note = 0;
        uint64_t program = 0;
        uint64_t expression = 0;
        int programs = 0;
        for (const seqlib::midi::Event& e : midi.tracks[t].events)
        {
            note = (e.bytes[0] == 0x9F && !note) ? e.tick : note;
            programs += e.bytes[0] == 0xCF ? 1 : 0;
            program = (e.bytes[0] == 0xCF && !program) ? e.tick : program;
            expression = (e.bytes[0] == 0xBF && e.bytes[1] == 11 && !expression) ? e.tick : expression;
        }

        SEQLIB_CHECK(note > 0 && program == note && expression == note);
        SEQLIB_CHECK_EQ(programs, 2);
    }
}

// With more than 15 channels that play notes, the channels whose settings clash least share a MIDI channel, even if
// their notes sound at once. A note ends where the mixer holds its levels at 0. Another track's expression could make
// it sound again. A note whose levels stay 0 throughout goes. With fewer channels, a note runs on through that silence.
void TestMidiChannelsFollowSettings()
{
    // Channels 1 to 14 hold notes on one key at full volume. Channels 15 and 16 hold notes on other keys at half
    // volume. Channel 4's levels go to 0 at row 2 and channel 15's at row 4. Channel 17's stay at 0.
    std::vector<Cell> cells;
    for (int c = 0; c < 14; c++)
    {
        cells.push_back({0, c, 49, 1, 0x50});
    }

    cells.push_back({0, 14, 53, 1, 0x30});
    cells.push_back({0, 15, 56, 1, 0x30});
    cells.push_back({0, 16, 49, 1, 0x10});
    cells.push_back({2, 3, -1, 0, 0x10});
    cells.push_back({4, 14, -1, 0, 0x10});
    Cart cart;
    cart.Module(17, 6, cells);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_settings";
    Cart few;
    few.Module(2, 6, {{0, 0, 49, 1}, {0, 1, 53, 1}, {2, 1, -1, 0, 0x10}});
    const Rom few_rom = few.ToRom();
    ConvertSettings few_opt = opt;
    few_opt.base_name = "krawall_settings_few";

    const SongReport report = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport few_report = ConvertSong(few_rom, Detect(few_rom), 0, few_opt, nullptr);

    SEQLIB_REQUIRE(report.ok && few_report.ok);
    SEQLIB_CHECK(report.warnings.empty());
    SEQLIB_CHECK_EQ(report.tracks, 16);
    const auto note_of = [](const agblib::testing::agb_files::MidiEvents& midi, const std::string& name)
    {
        const std::vector<std::string> names = seqlib::midi::TrackNames(midi);
        const size_t t = size_t(std::find(names.begin(), names.end(), name) - names.begin());
        std::array<uint64_t, 3> note{0, 0, 0}; // the first note on's status, and the end of the first note
        if (t >= midi.tracks.size())
        {
            return note;
        }

        for (const seqlib::midi::Event& e : midi.tracks[t].events)
        {
            if ((e.bytes[0] & 0xF0) == 0x90 && !note[0])
            {
                note[0] = e.bytes[0];
            }

            if ((e.bytes[0] & 0xF0) == 0x80 && !note[1])
            {
                note[1] = 1;
                note[2] = e.tick;
            }
        }

        return note;
    };
    const agblib::testing::agb_files::MidiEvents midi = ReadMidi(report.midi_path);
    const auto first = note_of(midi, "Channel 1");
    const auto faded = note_of(midi, "Channel 4");
    const auto fifteenth = note_of(midi, "Channel 15");
    const auto sixteenth = note_of(midi, "Channel 16");
    SEQLIB_CHECK(fifteenth[0] != 0 && fifteenth[0] == sixteenth[0]);
    SEQLIB_CHECK(first[0] != fifteenth[0]);
    SEQLIB_CHECK_EQ(note_of(midi, "Channel 17")[0], 0u);
    SEQLIB_CHECK(faded[2] > 0 && faded[2] < first[2]);

    // Where channel 15's note ends, channel 16's note sets its expression again.
    const std::vector<std::string> names = seqlib::midi::TrackNames(midi);
    const size_t sharer = size_t(std::find(names.begin(), names.end(), "Channel 16") - names.begin());
    SEQLIB_REQUIRE(sharer < midi.tracks.size());
    int set_again = 0;
    for (const seqlib::midi::Event& e : midi.tracks[sharer].events)
    {
        set_again += (e.bytes[0] & 0xF0) == 0xB0 && e.bytes[1] == 11 && e.tick == fifteenth[2] ? 1 : 0;
    }

    SEQLIB_CHECK(fifteenth[2] > 0 && fifteenth[2] < sixteenth[2]);
    SEQLIB_CHECK_EQ(set_again, 1);

    const agblib::testing::agb_files::MidiEvents few_midi = ReadMidi(few_report.midi_path);
    SEQLIB_CHECK_EQ(note_of(few_midi, "Channel 2")[2], note_of(few_midi, "Channel 1")[2]);
}

} // namespace

int RunTests()
{
    TestBars();
    TestLoopStartsAtSecondPass();
    TestManyChannels();
    TestSharedMidiChannel();
    TestMidiChannelsFollowSettings();

    return seqlib::testing::Summary("krawall_midi");
}

} // namespace agblib::sequence_export::krawall

int main()
{
    return agblib::sequence_export::krawall::RunTests();
}
