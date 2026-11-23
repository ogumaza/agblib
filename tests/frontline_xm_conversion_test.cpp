// SPDX-License-Identifier: MIT

// Unit tests for the conversion of Frontline Studios' XM-like modules to MIDI and SoundFont files. They build a
// synthetic cartridge in memory, and read no game's ROM. Each expected value follows from the test module's rows and
// instruments, from the data's tables and from the driver's rules that the comments of `agblib/frontline_xm/player.h`
// and `driver.h` give: a tick lasts 20068 x 5 / (2 x BPM) samples, the play leaves the first row after the speed's
// ticks, a period is 7680 - 64 x note - finetune / 2, and a channel's gain is its volume x the master volume, of the
// 2^14 that plays a point at full scale.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/testing/check.h"
#include "xsflib/testing/near.h"

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/frontline_xm/convert.h"
#include "agblib/sequence_export/frontline_xm/soundfont.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/frontline_xm_cart.h"
#include "frontline_xm_test_util.h"
#include "module_conversion_test_util.h"

namespace agblib::sequence_export::frontline_xm
{
namespace
{

namespace cart = agblib::testing::frontline_xm_cart;

using agblib::agb::kRomBase;
using agblib::frontline_xm::kEffectOffset;
using agblib::frontline_xm::Module;
using agblib::frontline_xm::ReadModule;
using agblib::testing::agb_files::TempSettings;
using agblib::testing::frontline_xm_tests::TestDriver;
using agblib::testing::module_conversion_tests::ChannelNotes;
using agblib::testing::module_conversion_tests::ConvertToMidi;
using agblib::testing::module_conversion_tests::CountGenerators;
using agblib::testing::module_conversion_tests::SetsLevel;
using xsflib::testing::Near;

// A tick at the data's BPM of 125, a row at its speed of 3, and a row's MIDI ticks.
constexpr double kTick = 20068 * 5 / (2 * 125);
constexpr double kRow = cart::kSpeed * kTick;
constexpr uint32_t kRowTicks = 120;

// The master volume of the tests' song.
constexpr uint8_t kMaster = 200;

// Returns a cartridge with the test data and a module at cart::kSpare whose one order plays `rows`.
Rom MakeRom(const std::vector<cart::Row>& rows, uint32_t& module)
{
    cart::Data data(kRomBase);
    module = kRomBase + data.PutModule(cart::kSpare, cart::kSpeed, {rows}, {0}) - cart::kHeaderSize;
    Rom rom;
    rom.Assign(data.Bytes());
    return rom;
}

// The driver as the model reads it: the data's tables, and the module at `address` as song 0, its list from order 0 at
// the master volume kMaster.
DriverInfo MakeInfo(const Rom& rom, uint32_t address)
{
    DriverInfo info = TestDriver();
    Module module;
    SEQLIB_CHECK(ReadModule(rom, address, module));
    info.modules.push_back(module);
    info.songs.push_back({0, module.orders, module.last_order, kMaster});
    return info;
}

double Seconds(double samples)
{
    return samples / cart::kRate;
}

// Converts song 0 in memory, and reads its MIDI file.
seqlib::midi::File Convert(const Rom& rom, const DriverInfo& info, SongReport& report)
{
    return ConvertToMidi(ConvertSongFiles, rom, info, report);
}

// The notes of a pattern of 8 rows: the triangle's note 48 on channel 0 at rows 0 and 4, and the saw's note 36 on
// channel 1 at row 0.
std::vector<cart::Row> NoteRows()
{
    const cart::Cell e;
    std::vector<cart::Row> rows(8, cart::Row{e, e, e, e});
    rows[0][0] = {48, cart::kTriangle};
    rows[0][1] = {36, cart::kSaw};
    rows[4][0] = {48, cart::kTriangle};
    return rows;
}

// A note starts at its row and ends at the next start of its sample. Note 48 of the triangle has a finetune of 0. It is
// the root period and plays key 60. The saw's finetune of 12 moves its root period to 4602, and its note 36 plays
// period 5370, 768 periods lower: the data's tables give half the step, key 48. The end of the order list goes back to
// order 0. The file marks that order as its loop and plays it twice. The song's first row comes after the speed's 3
// ticks. The triangle's volume of 64 at the master volume of 200 gives a level of 64 x 200 / 2^14 x 128 = 100, and the
// saw's volume of 50 a level of 78.125.
void TestNotesAndLevels()
{
    uint32_t module = 0;
    const Rom rom = MakeRom(NoteRows(), module);
    SongReport report;
    const seqlib::midi::File midi = Convert(rom, MakeInfo(rom, module), report);
    const std::vector<seqlib::midi::Note> first = ChannelNotes(midi, 0);
    const std::vector<seqlib::midi::Note> second = ChannelNotes(midi, 1);
    const std::vector<seqlib::midi::Text> markers = seqlib::midi::Markers(midi);

    SEQLIB_CHECK_EQ(report.tracks, 2);
    SEQLIB_REQUIRE_EQ(first.size(), size_t(4));
    for (size_t k = 0; k < first.size(); k++)
    {
        SEQLIB_CHECK(first[k].key == 60 && first[k].on == 4 * kRowTicks * k && first[k].off == 4 * kRowTicks * (k + 1));
    }

    SEQLIB_REQUIRE_EQ(second.size(), size_t(2));
    SEQLIB_CHECK(second[0].key == 48 && second[0].on == 0 && second[0].off == 8 * kRowTicks);
    SEQLIB_CHECK(second[1].on == 8 * kRowTicks && second[1].off == 16 * kRowTicks);
    SEQLIB_CHECK(seqlib::midi::Bends(midi).empty());
    SEQLIB_REQUIRE_EQ(markers.size(), size_t(2));
    SEQLIB_CHECK(markers[0].text == "loopStart" && markers[0].tick == 0);
    SEQLIB_CHECK(markers[1].text == "loopEnd" && markers[1].tick == 8 * kRowTicks);
    SEQLIB_CHECK(Near(report.loop_start, Seconds(kRow), 1e-6) && Near(report.loop_end, Seconds(9 * kRow), 1e-6));
    SEQLIB_CHECK(Near(report.seconds, Seconds(17 * kRow), 1e-6));
    SEQLIB_CHECK(SetsLevel(midi, 0, 100));
    SEQLIB_CHECK(SetsLevel(midi, 1, 78.125));
}

// A sample offset of 1 starts the noise 256 points in: a preset of its own, whose zone starts there, on the SoundFont
// sample that the note from the start on channel 1 plays too.
void TestOffset()
{
    const cart::Cell e;
    std::vector<cart::Row> rows(4, cart::Row{e, e, e, e});
    rows[0][0] = {48, cart::kNoise, -1, kEffectOffset, 1};
    rows[0][1] = {48, cart::kNoise};
    uint32_t module = 0;
    const Rom rom = MakeRom(rows, module);

    InstrumentSet shared;
    const SongReport report = ConvertSong(rom, MakeInfo(rom, module), 0, TempSettings("frontline_xm_offset"), &shared);
    const Sf2File sf = shared.Build(rom);

    SEQLIB_REQUIRE(report.ok && report.tracks == 2);
    SEQLIB_CHECK_EQ(sf.samples.size(), size_t(1));
    SEQLIB_REQUIRE_EQ(sf.presets.size(), size_t(2));
    SEQLIB_CHECK_EQ(CountGenerators(sf, seqlib::sf2::kStartAddrsOffset, 256), 1);
}

// The mixer plays the triangle's loop from point 16 up to point 63, and carries a position's fraction past the end to
// the loop's start. The SoundFont sample therefore has 64 points before the conversion lengthens its loop. The loop
// repeats every 48 points. The root period's step in the data's tables is 0x4FF00 >> 6 = 5116. That step plays 5116 x
// 20068 / 8192 = 12533 points a second.
void TestLoopedSample()
{
    uint32_t module = 0;
    const Rom rom = MakeRom(NoteRows(), module);
    const DriverInfo info = MakeInfo(rom, module);
    const uint32_t record = rom.U32(info.modules[0].instruments + 4 * cart::kTriangle);
    PlayedSample played;
    played.points = record + agblib::frontline_xm::kInstrumentPoints;
    played.length = 64;
    played.loop_start = 16;
    played.loop_end = 64;
    played.loops = true;
    played.rate = uint32_t(std::lround(RootRate(rom, info, 0)));

    InstrumentSet set;
    set.Program(played, cart::kTriangle);
    const Sf2File sf = set.Build(rom);

    SEQLIB_REQUIRE_EQ(sf.samples.size(), size_t(1));
    const seqlib::sf2::Sample& out = sf.samples[0];
    SEQLIB_CHECK_EQ(out.sample_rate, 12533u);
    SEQLIB_CHECK_EQ(out.loop_start, 16u);
    SEQLIB_CHECK_EQ((out.loop_end - out.loop_start) % 48, 0u);
    SEQLIB_REQUIRE(out.data.size() > 64);
    bool same = true;
    for (uint32_t i = 0; i < 64; i++)
    {
        same = same && out.data[i] == rom.S8(played.points + i) * 256;
    }

    SEQLIB_CHECK(same);
    SEQLIB_CHECK_EQ(out.data[64], out.data[16]);
}

} // namespace

int RunTests()
{
    TestNotesAndLevels();
    TestOffset();
    TestLoopedSample();

    return seqlib::testing::Summary("frontline_xm_conversion");
}

} // namespace agblib::sequence_export::frontline_xm

int main()
{
    return agblib::sequence_export::frontline_xm::RunTests();
}
