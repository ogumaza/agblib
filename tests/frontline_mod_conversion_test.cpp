// SPDX-License-Identifier: MIT

// Unit tests for the conversion of Frontline Studios' modules to MIDI and SoundFont files. They build a synthetic
// cartridge in memory, and read no game's ROM. Each expected value follows from the test module's rows and samples and
// from the driver's rules that the comments of `agblib/frontline_mod/player.h` and `driver.h` give: a row lasts the
// speed's frames of 304 samples, a channel's level is its volume x the music's volume >> 8, its gain is the level x the
// master volume >> 6, and the clip table of scale 64 halves each sum.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/midi/builder.h"
#include "seqlib/midi/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/testing/check.h"
#include "xsflib/testing/near.h"

#include "agblib/agb/rom.h"
#include "agblib/frontline_mod/driver.h"
#include "agblib/frontline_mod/player.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/frontline_mod/convert.h"
#include "agblib/sequence_export/frontline_mod/soundfont.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/frontline_mod_cart.h"
#include "module_conversion_test_util.h"

namespace agblib::sequence_export::frontline_mod
{
namespace
{

namespace cart = agblib::testing::frontline_mod_cart;

using agblib::agb::kRomBase;
using agblib::frontline_mod::kEffectSampleOffset;
using agblib::frontline_mod::kFullVolume;
using agblib::frontline_mod::kInstrumentLoopEnd;
using agblib::frontline_mod::kInstrumentLoopStart;
using agblib::frontline_mod::kInstrumentSample;
using agblib::frontline_mod::kSampleLength;
using agblib::frontline_mod::kSamplePoints;
using agblib::frontline_mod::Module;
using agblib::frontline_mod::Player;
using agblib::frontline_mod::ReadModule;
using agblib::frontline_mod::Song;
using agblib::testing::agb_files::TempSettings;
using agblib::testing::module_conversion_tests::ChannelNotes;
using agblib::testing::module_conversion_tests::ConvertToMidi;
using agblib::testing::module_conversion_tests::CountGenerators;
using agblib::testing::module_conversion_tests::SetsLevel;
using xsflib::testing::Near;

constexpr uint32_t kIwramBase = 0x03000000;

// The test's table of periods, after the data: the conversion's root index plays 8,363 points a second.
static_assert(cart::kRootIndex == kRootIndex);
constexpr uint32_t kOctaveIndex = cart::kOctaveIndex;
constexpr uint32_t kFlatIndex = cart::kFlatIndex;

// The test module's speed, and the samples of a row: the speed's frames of 304 samples.
constexpr uint32_t kSpeed = 6;
constexpr double kRow = kSpeed * 304;
constexpr uint32_t kRowTicks = 120;

// A pitch bend's value at the centre.
constexpr int kBendCentre = 8192;

// The mixer's settings of the model: master volume 0x50, clip scale 64.
constexpr uint32_t kMaster = 0x50;

// Returns a cartridge with the test data, the second table of periods and a module at cart::kSpare whose one order
// plays `rows`.
Rom MakeRom(const std::vector<cart::Row>& rows)
{
    cart::Data data(kRomBase);
    data.PutModule(cart::kSpare, kSpeed, {rows}, {0});
    Rom rom;
    rom.Assign(cart::WithGameTable(data.Bytes()));
    return rom;
}

// The driver as the model reads it: the test's table of periods, the data's vibrato table, and the module at
// cart::kSpare as song 0, started with `step`.
DriverInfo MakeInfo(const Rom& rom, int32_t step)
{
    DriverInfo info;
    info.periods = kRomBase + cart::kGameTable;
    info.vibrato = kIwramBase;
    info.image_base = kRomBase + cart::kVibrato;
    info.copy_start = kIwramBase;
    info.copy_end = kIwramBase + 64;
    Module module;
    SEQLIB_CHECK(ReadModule(rom, kRomBase + cart::kSpare, module));
    info.modules.push_back(module);
    Song song;
    song.step = step;
    info.songs.push_back(song);

    return info;
}

// Returns the seconds of `samples` of the mixer.
double Seconds(double samples)
{
    return samples / DriverInfo().Rate();
}

// Converts song 0 in memory, and reads its MIDI file.
seqlib::midi::File Convert(const Rom& rom, const DriverInfo& info, SongReport& report)
{
    return ConvertToMidi(ConvertSongFiles, rom, info, report);
}

// The notes of a pattern of 8 rows: the triangle on channel 0 at rows 0 and 4, and the saw an octave up on channel 1
// at row 0.
std::vector<cart::Row> NoteRows()
{
    const cart::Cell e;
    std::vector<cart::Row> rows(8, cart::Row{e, e, e, e});
    rows[0][0] = {int(kRootIndex), cart::kTriangle};
    rows[0][1] = {int(kOctaveIndex), cart::kSaw};
    rows[4][0] = {int(kRootIndex), cart::kTriangle};
    return rows;
}

// A note starts at its row and ends at the next start of its sample. The root index plays key 60, and the index an
// octave above it key 72, both in tune at the note's start: the bends of the loops' passes come later (TestLoopPasses).
// The end of the orders goes back to order 0. The file marks that order as its loop and plays it twice. The triangle's
// volume of 64 at the music's full volume gives a level of 64 and a gain of 64 x 0x50 >> 6 = 80, of the 128 that plays
// a point at full scale. The saw's volume of 50 gives a gain of 50 x 0x50 >> 6 = 62.
void TestNotesAndLevels()
{
    const Rom rom = MakeRom(NoteRows());
    SongReport report;
    const seqlib::midi::File midi = Convert(rom, MakeInfo(rom, kFullVolume), report);
    const std::vector<seqlib::midi::Note> first = ChannelNotes(midi, 0);
    const std::vector<seqlib::midi::Note> second = ChannelNotes(midi, 1);
    const std::vector<seqlib::midi::Text> markers = seqlib::midi::Markers(midi);
    const std::vector<seqlib::midi::Bend> bends = seqlib::midi::Bends(midi);

    SEQLIB_CHECK_EQ(report.tracks, 2);
    SEQLIB_REQUIRE_EQ(first.size(), size_t(4));
    for (size_t k = 0; k < first.size(); k++)
    {
        SEQLIB_CHECK(first[k].key == 60 && first[k].on == 4 * kRowTicks * k && first[k].off == 4 * kRowTicks * (k + 1));
    }

    SEQLIB_REQUIRE_EQ(second.size(), size_t(2));
    SEQLIB_CHECK(second[0].key == 72 && second[0].on == 0 && second[0].off == 8 * kRowTicks);
    SEQLIB_CHECK(second[1].on == 8 * kRowTicks && second[1].off == 16 * kRowTicks);
    SEQLIB_CHECK(std::none_of(bends.begin(), bends.end(),
                              [](const seqlib::midi::Bend& b) { return b.tick == 0 && b.value != kBendCentre; }));
    SEQLIB_REQUIRE_EQ(markers.size(), size_t(2));
    SEQLIB_CHECK(markers[0].text == "loopStart" && markers[0].tick == 0);
    SEQLIB_CHECK(markers[1].text == "loopEnd" && markers[1].tick == 8 * kRowTicks);
    SEQLIB_CHECK(report.loop_start == 0 && Near(report.loop_end, Seconds(8 * kRow), 1e-6));
    SEQLIB_CHECK(Near(report.seconds, Seconds(16 * kRow), 1e-6));
    SEQLIB_CHECK(SetsLevel(midi, 0, 64 * kMaster >> 6));
    SEQLIB_CHECK(SetsLevel(midi, 1, 50 * kMaster >> 6));
}

// A start with a fade step of 64 raises the music's volume by 64 at each row: the triangle's levels are 16, 32, 48 and
// 64, and its gains 20, 40, 60 and 80. The volume is full from row 3 on. The loop therefore starts there, and its end
// comes back to row 3 in the second pass through the pattern.
void TestFadeIn()
{
    const Rom rom = MakeRom(NoteRows());
    SongReport report;
    const seqlib::midi::File midi = Convert(rom, MakeInfo(rom, 64), report);
    const std::vector<seqlib::midi::Text> markers = seqlib::midi::Markers(midi);

    for (const int gain : {20, 40, 60, 80})
    {
        SEQLIB_CHECK(SetsLevel(midi, 0, gain));
    }

    SEQLIB_REQUIRE_EQ(markers.size(), size_t(2));
    SEQLIB_CHECK(markers[0].tick == 3 * kRowTicks && markers[1].tick == 11 * kRowTicks);
    SEQLIB_CHECK(Near(report.loop_start, Seconds(3 * kRow), 1e-6) && Near(report.loop_end, Seconds(11 * kRow), 1e-6));
    SEQLIB_CHECK(Near(report.seconds, Seconds(19 * kRow), 1e-6));
}

// A note a quarter of a semitone below the root index plays key 60 with a bend down from its start.
void TestBend()
{
    const cart::Cell e;
    std::vector<cart::Row> rows(4, cart::Row{e, e, e, e});
    rows[0][0] = {int(kFlatIndex), cart::kTriangle};
    const Rom rom = MakeRom(rows);
    SongReport report;
    const seqlib::midi::File midi = Convert(rom, MakeInfo(rom, kFullVolume), report);
    const std::vector<seqlib::midi::Note> notes = ChannelNotes(midi, 0);
    const std::vector<seqlib::midi::Bend> bends = seqlib::midi::Bends(midi);

    SEQLIB_REQUIRE(!notes.empty() && !bends.empty());
    SEQLIB_CHECK_EQ(notes[0].key, 60);
    SEQLIB_CHECK(std::any_of(bends.begin(), bends.end(),
                             [](const seqlib::midi::Bend& b) { return b.tick == 0 && b.value < kBendCentre; }));
}

// The mixer checks the triangle's loop end once every two samples and restarts the loop with the fraction at 0. At the
// root index's step of 30185, a pass through its 65 points lasts 142 samples, the first even count whose points pass
// the loop's end: 142 x 30185 >= 65 x 65536 > 140 x 30185. The loop therefore plays 65 points in 142 samples where the
// step plays 142 x 30185 / 65536 = 65.4. The first pass ends in the first frame. From the second frame, at MIDI tick
// 20, the note bends down by 12 x log2(65 x 65536 / (142 x 30185)) = -0.107 semitones, at a bend range of 2.
void TestLoopPasses()
{
    const cart::Cell e;
    std::vector<cart::Row> rows(4, cart::Row{e, e, e, e});
    rows[0][0] = {int(kRootIndex), cart::kTriangle};
    const Rom rom = MakeRom(rows);
    SongReport report;
    const seqlib::midi::File midi = Convert(rom, MakeInfo(rom, kFullVolume), report);
    const std::vector<seqlib::midi::Note> notes = ChannelNotes(midi, 0);
    const std::vector<seqlib::midi::Bend> bends = seqlib::midi::Bends(midi);
    const double semitones = 12 * std::log2(65.0 * 65536 / (142.0 * cart::kRootStep));
    const int value = agblib::sequence_export::agb::BendValue(semitones, 2);

    SEQLIB_REQUIRE(!notes.empty() && !bends.empty());
    SEQLIB_CHECK_EQ(notes[0].key, 60);
    SEQLIB_CHECK(value < kBendCentre);
    SEQLIB_CHECK(std::none_of(bends.begin(), bends.end(),
                              [](const seqlib::midi::Bend& b) { return b.tick == 0 && b.value != kBendCentre; }));
    SEQLIB_CHECK(std::any_of(bends.begin(), bends.end(), [&](const seqlib::midi::Bend& b)
                             { return b.tick == kRowTicks / kSpeed && b.value == value; }));
}

// A sample offset of 1 starts the noise 256 points in: a preset of its own, whose zone starts there, on the SoundFont
// sample that the note from the start on channel 1 plays too. From point 256, the noise's 400 points last 144 / 0.46 =
// 313 samples at the root index: the note ends in the second frame's mix, and the third frame of six, at MIDI tick 40,
// finds it stopped.
void TestOffset()
{
    const cart::Cell e;
    std::vector<cart::Row> rows(4, cart::Row{e, e, e, e});
    rows[0][0] = {int(kRootIndex), cart::kNoise, false, -1, kEffectSampleOffset, 1};
    rows[0][1] = {int(kRootIndex), cart::kNoise};
    const Rom rom = MakeRom(rows);

    InstrumentSet shared;
    const SongReport report =
        ConvertSong(rom, MakeInfo(rom, kFullVolume), 0, TempSettings("frontline_mod_offset"), &shared);
    const Sf2File sf = shared.Build(rom);

    SEQLIB_REQUIRE(report.ok && report.tracks == 2);
    SEQLIB_CHECK_EQ(sf.samples.size(), size_t(1));
    SEQLIB_REQUIRE_EQ(sf.presets.size(), size_t(2));
    SEQLIB_CHECK_EQ(CountGenerators(sf, seqlib::sf2::kStartAddrsOffset, 256), 1);

    SongReport inspected;
    const seqlib::midi::File midi = Convert(rom, MakeInfo(rom, kFullVolume), inspected);
    const std::vector<seqlib::midi::Note> notes = ChannelNotes(midi, 0);
    SEQLIB_REQUIRE(!notes.empty());
    SEQLIB_CHECK(notes[0].on == 0 && notes[0].off == 2 * kRowTicks / kSpeed);
}

// A sample offset starts the sample again. The conversion therefore counts the loop's restarts from 0 again: the bend
// to the loop's pitch waits for the next restart. At the root index, the mixer plays about 140 of the saw's points in a
// frame, and its loop of 201 points restarts more than once during row 0. Row 1's offset of 256 points moves the
// position past the loop's end, and that frame's mix restarts the loop once.
void TestOffsetCountsRestartsAgain()
{
    const cart::Cell e;
    std::vector<cart::Row> rows(4, cart::Row{e, e, e, e});
    rows[0][0] = {int(kRootIndex), cart::kSaw};
    rows[1][0] = {-1, 0, false, -1, kEffectSampleOffset, 1};
    const Rom rom = MakeRom(rows);
    const DriverInfo info = MakeInfo(rom, kFullVolume);
    Player player(rom, info);
    player.Start(info.modules[0], 0, kFullVolume);
    for (uint32_t frame = 0; frame < kSpeed; frame++)
    {
        player.Frame();
    }

    const uint32_t before = player.ChannelAt(0).wraps;

    player.Frame();

    SEQLIB_CHECK(before > 1);
    SEQLIB_CHECK_EQ(player.ChannelAt(0).starts, 2u);
    SEQLIB_CHECK_EQ(player.ChannelAt(0).wraps, 1u);
}

// The mixer plays the saw's loop from point 100 up to and including point 300, the byte after the sample. The SoundFont
// sample therefore has 301 points before the conversion lengthens its loop. The loop repeats every 201 points. The root
// index's step plays 30185 x 18157.2 / 65536 = 8363 points a second.
void TestLoopedSample()
{
    const Rom rom = MakeRom(NoteRows());
    const uint32_t record = rom.U32(kRomBase + cart::kInstruments + 4 * cart::kSaw);
    const uint32_t sample = rom.U32(record + kInstrumentSample);
    PlayedSample played;
    played.points = rom.U32(sample + kSamplePoints);
    played.length = rom.U32(sample + kSampleLength);
    played.loop_start = rom.U32(record + kInstrumentLoopStart);
    played.loop_end = rom.U32(record + kInstrumentLoopEnd);
    played.rate = uint32_t(std::lround(RootRate(rom, MakeInfo(rom, kFullVolume))));

    InstrumentSet set;
    set.Program(played, cart::kSaw);
    const Sf2File sf = set.Build(rom);

    SEQLIB_REQUIRE_EQ(sf.samples.size(), size_t(1));
    const seqlib::sf2::Sample& out = sf.samples[0];
    SEQLIB_CHECK_EQ(played.length, 300u);
    SEQLIB_CHECK_EQ(out.sample_rate, 8363u);
    SEQLIB_CHECK_EQ(out.loop_start, 100u);
    SEQLIB_CHECK_EQ((out.loop_end - out.loop_start) % 201, 0u);
    SEQLIB_REQUIRE(out.data.size() > 301);
    bool same = true;
    for (uint32_t i = 0; i <= 300; i++)
    {
        same = same && out.data[i] == rom.S8(played.points + i) * 256;
    }

    SEQLIB_CHECK(same);
    SEQLIB_CHECK_EQ(out.data[301], out.data[100]);
}

} // namespace

int RunTests()
{
    TestNotesAndLevels();
    TestFadeIn();
    TestBend();
    TestLoopPasses();
    TestOffset();
    TestOffsetCountsRestartsAgain();
    TestLoopedSample();

    return seqlib::testing::Summary("frontline_mod_conversion");
}

} // namespace agblib::sequence_export::frontline_mod

int main()
{
    return agblib::sequence_export::frontline_mod::RunTests();
}
