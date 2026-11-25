// SPDX-License-Identifier: MIT

// Unit tests for the conversion of Engine Software's songs to MIDI and SoundFont files. They use the synthetic
// cartridge of testing::engine_software_cart and read no game's ROM. Each expected value follows from the cartridge's
// rows and instruments and from the driver's rules that the comments of `agblib/engine_software/driver.h` and
// `player.h` give: a tick lasts the rate over (tempo x 0x333 x 0x1001) >> 23 samples, a note's index is (relative note
// + note) x 128 + fine tune, the step is the pitch table's rate at the index times 0x4E6 >> 8, and a channel's level is
// (volume x music volume >> 8) x envelope >> 6, of the quarter of the clip table's slope that plays a point at full
// scale.

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/testing/check.h"
#include "xsflib/testing/near.h"

#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/engine_software/convert.h"
#include "agblib/sequence_export/engine_software/soundfont.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/engine_software_cart.h"
#include "module_conversion_test_util.h"

namespace agblib::sequence_export::engine_software
{
namespace
{

namespace cart = agblib::testing::engine_software_cart;

using agblib::engine_software::Builds;
using agblib::engine_software::kInstrumentSamples;
using agblib::testing::agb_files::TempSettings;
using agblib::testing::module_conversion_tests::ChannelNotes;
using agblib::testing::module_conversion_tests::ConvertToMidi;
using agblib::testing::module_conversion_tests::SetsLevel;
using xsflib::testing::Near;

// The cartridge's songs.
constexpr int kJingle = 1;
constexpr int kShort = 2;

// The mixer's rate: the timer's clock of 2^24 Hz over 2^24 / 13,378 rounded down.
constexpr double kRate = 16777216.0 / 1254;

// The step's factor at 13,378 Hz.
constexpr uint32_t kStepFactor = 0x4E6;

// The music volume that the game's sound manager sets.
constexpr int kMusicVolume = 0xD2;

Rom MakeRom()
{
    Rom rom;
    rom.Assign(cart::Image(Builds().front()));
    return rom;
}

// The driver with the cartridge's song `song` alone, as song 0.
DriverInfo SongInfo(const Rom& rom, int song)
{
    DriverInfo info = cart::TestInfo(rom, Builds().front());
    info.songs = {info.songs[size_t(song)]};
    return info;
}

// The samples of a tick at `tempo`.
double TickSamples(int tempo)
{
    return std::floor(13378.0 / double((uint64_t(tempo) * 0x333 * 0x1001) >> 23));
}

// The rate in hertz at which the mixer plays the pitch table's entry at `index`.
double IndexRate(uint32_t index)
{
    return double((kStepFactor * cart::PitchAt(index)) >> 8) * kRate / 65536;
}

// The short song plays 3 channels for 2 orders of 4 rows, at speed 2 and tempo 200: 169 samples a tick. The game starts
// it at its music volume at once. Note 49 of instrument 4 plays key 60, the root key with the instrument's relative
// note. Note 37 of instrument 1 plays key 48, and note 61 of instrument 3 key 72. The list's return to order 0 marks
// the loop from the first row: 8 rows of 120 MIDI ticks. Instrument 1's volume of 48 at the music volume gives a level
// of (48 x 210 >> 8) x 64 >> 6 = 39, and the clip table of 3 channels has a slope of 512. 39 is therefore the
// expression's level. The game mixes mono: every note plays at the centre.
void TestNotesAndLevels()
{
    const Rom rom = MakeRom();
    SongReport report;
    const seqlib::midi::File midi = ConvertToMidi(ConvertSongFiles, rom, SongInfo(rom, kShort), report);
    const std::vector<seqlib::midi::Text> markers = seqlib::midi::Markers(midi);
    const double row = 2 * TickSamples(200);

    SEQLIB_CHECK_EQ(report.tracks, 3);
    for (const auto& [channel, key] : {std::pair{0, 60}, std::pair{1, 48}, std::pair{2, 72}})
    {
        const std::vector<seqlib::midi::Note> notes = ChannelNotes(midi, channel);
        SEQLIB_REQUIRE(!notes.empty());
        SEQLIB_CHECK(notes[0].key == key && notes[0].on == 0);
    }

    SEQLIB_REQUIRE_EQ(markers.size(), size_t(2));
    SEQLIB_CHECK(markers[0].text == "loopStart" && markers[0].tick == 0);
    SEQLIB_CHECK(markers[1].text == "loopEnd" && markers[1].tick == 8 * 120);
    SEQLIB_CHECK(Near(report.loop_end - report.loop_start, 8 * row / kRate, 1e-6));
    SEQLIB_CHECK(SetsLevel(midi, 1, (((48 * kMusicVolume) >> 8) * 64) >> 6));
    for (int channel = 0; channel < 3; channel++)
    {
        for (const seqlib::midi::Event& e : midi.Find(uint8_t(0xB0 + channel)))
        {
            SEQLIB_CHECK(e.bytes[1] != 10 || e.bytes[2] == 64);
        }
    }
}

// The jingle plays its pattern of 8 rows once, at speed 5 and tempo 125: 273 samples a tick. The driver takes the next
// order as it reads row 7, the pattern's last, and past the last order it stops the jingle at the end of that frame.
// The song therefore ends at row 7's start, with no loop.
void TestJingleEnds()
{
    const Rom rom = MakeRom();
    SongReport report;
    ConvertToMidi(ConvertSongFiles, rom, SongInfo(rom, kJingle), report);

    SEQLIB_CHECK(report.loop_start < 0);
    SEQLIB_CHECK(Near(report.seconds, 7 * 5 * TickSamples(125) / kRate, 1e-6));
}

// A song that the game plays for one pass: the short song with the mark. The driver goes back to order 0 after the
// first pass's 8 rows, and the song ends there, at the loop's end of TestNotesAndLevels(), with no loop.
void TestOnePassEnds()
{
    const Rom rom = MakeRom();
    DriverInfo info = SongInfo(rom, kShort);
    info.songs[0].start.one_pass = true;
    SongReport report;
    const seqlib::midi::File midi = ConvertToMidi(ConvertSongFiles, rom, info, report);

    SEQLIB_CHECK(report.loop_start < 0);
    SEQLIB_CHECK(seqlib::midi::Markers(midi).empty());
    SEQLIB_CHECK(Near(report.seconds, 8 * 2 * TickSamples(200) / kRate, 1e-6));
}

// The SoundFont of the short song. Instrument 1 loops from point 8 for 56 points, up to its end at 64: the SoundFont
// sample has its 64 points before the conversion lengthens the loop. Its root plays at the rate of index 49 x 128.
// Instrument 4 has a relative note of -12 and a fine tune of 30: index 37 x 128 + 30.
void TestSamples()
{
    const Rom rom = MakeRom();
    const DriverInfo info = SongInfo(rom, kShort);
    InstrumentSet shared;
    const SongReport report = ConvertSong(rom, info, 0, TempSettings("engine_software_samples"), &shared);
    const Sf2File sf = shared.Build(rom);

    SEQLIB_REQUIRE(report.ok && report.tracks == 3);
    SEQLIB_REQUIRE_EQ(sf.samples.size(), size_t(3));
    const seqlib::sf2::Sample* looped = nullptr;
    const seqlib::sf2::Sample* tuned = nullptr;
    for (const seqlib::sf2::Sample& s : sf.samples)
    {
        looped = s.name == "Sample 01" ? &s : looped;
        tuned = s.name == "Sample 04" ? &s : tuned;
    }

    SEQLIB_REQUIRE(looped && tuned);
    const uint32_t points = info.bank.instruments[0] + kInstrumentSamples;
    SEQLIB_CHECK_EQ(looped->sample_rate, uint32_t(std::lround(IndexRate(49 * 128))));
    SEQLIB_CHECK_EQ(looped->loop_start, 8u);
    SEQLIB_CHECK_EQ((looped->loop_end - looped->loop_start) % 56, 0u);
    SEQLIB_REQUIRE(looped->data.size() > 64);
    bool same = true;
    for (uint32_t i = 0; i < 64; i++)
    {
        same = same && looped->data[i] == rom.S8(points + i) * 256;
    }

    SEQLIB_CHECK(same);
    SEQLIB_CHECK_EQ(looped->data[64], looped->data[8]);
    SEQLIB_CHECK_EQ(tuned->sample_rate, uint32_t(std::lround(IndexRate(37 * 128 + 30))));
}

int RunTests()
{
    TestNotesAndLevels();
    TestJingleEnds();
    TestOnePassEnds();
    TestSamples();

    return seqlib::testing::Summary("engine_software_conversion");
}

} // namespace
} // namespace agblib::sequence_export::engine_software

int main()
{
    return agblib::sequence_export::engine_software::RunTests();
}
