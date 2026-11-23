// SPDX-License-Identifier: MIT

// Unit tests for the conversion of Webfoot Technologies' songs to MIDI and SoundFont files. They build small synthetic
// cartridges in memory, and read no game's ROM. Each expected value follows from the test module's rows and samples and
// from the driver's rules that the comments of `agblib/webfoot/player.h` give: a tick lasts 40,000 / tempo samples in
// the 2003 build, a row lasts the speed's ticks, a side's level is the music's volume × the volume × (64 ∓ pan) >> 10,
// and a sample plays its rate at the middle note.

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
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/webfoot/convert.h"
#include "agblib/sequence_export/webfoot/soundfont.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/webfoot_cart.h"
#include "agblib/webfoot/driver.h"
#include "module_conversion_test_util.h"

namespace agblib::sequence_export::webfoot
{
namespace
{

using agblib::testing::agb_files::TempSettings;
using agblib::testing::module_conversion_tests::ChannelNotes;
using agblib::testing::module_conversion_tests::ConvertToMidi;
using agblib::testing::module_conversion_tests::CountGenerators;
using agblib::webfoot::Build;
using agblib::webfoot::DetectDriver;
using agblib::webfoot::DriverInfo;
using agblib::webfoot::kSampleLoops;
using agblib::webfoot::kSamplePingPong;
using agblib::webfoot::kSampleSize;
using agblib::webfoot::kStartMusicVolume;
using agblib::webfoot::MixerOf;
using agblib::webfoot::ReadSample;
using agblib::webfoot::Sample;
using namespace agblib::testing::webfoot_cart;
using xsflib::testing::Near;

// The MIDI ticks of a row, and a row's samples at speed 6 and tempo 125 in the 2003 build: 6 ticks of 40,000 / 125.
constexpr uint32_t kRowTicks = 120;
constexpr uint32_t kRow = 6 * (40000 / 125);

// The test modules' samples: a forward loop over 1000 points, the default of TestSample, a sample of 100 points without
// a loop, and a sawtooth of 1000 points with a ping-pong loop over its last 500.
constexpr uint8_t kLooped = 0;
constexpr uint8_t kOneShot = 1;
constexpr uint8_t kPingPong = 2;

// The effects, by Impulse Tracker's letters: B, O, Q, T and X, and the note delay.
constexpr uint8_t kJump = 2;
constexpr uint8_t kOffset = 15;
constexpr uint8_t kRetrigger = 17;
constexpr uint8_t kTempo = 20;
constexpr uint8_t kPan = 24;
constexpr uint8_t kDelay = 27;

// A pan's parameter for the left side alone: -64.
constexpr uint8_t kLeft = 0xC0;

// Returns a cartridge of `build` with the test samples.
TestCart Cart(Build build)
{
    TestCart cart(build);
    cart.AddSample({});
    cart.AddSample({64, 100, 0, 0, 16000});
    cart.AddSample({64, 1000, kSampleLoops | kSamplePingPong, 500, 16000, true});

    return cart;
}

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));

    return info;
}

// Returns the seconds of `samples` of the 2003 build's mixer.
double Seconds(double samples)
{
    return samples / MixerOf(Build::k2003).Rate();
}

// Converts the first song of `rom` in memory, and reads its MIDI file.
seqlib::midi::File Convert(const Rom& rom, SongReport& report)
{
    return ConvertToMidi(ConvertSongFiles, rom, Detect(rom), report);
}

// Returns true if the file sets controller `controller` of MIDI channel `channel` to `value`.
bool Sets(const seqlib::midi::File& midi, int channel, uint8_t controller, int value)
{
    const auto sets = [&](const seqlib::midi::Event& e)
    {
        return e.bytes[1] == controller && e.bytes[2] == value;
    };
    const std::vector<seqlib::midi::Event> events = midi.Find(uint8_t(0xB0 + channel));

    return std::any_of(events.begin(), events.end(), sets);
}

// A note starts at its row and ends at a note off or at the next start of its sample. Each voice's key is its note: a
// sample plays its rate at the middle note, and the SoundFont sample has that rate. The levels of the two sides set the
// pan and the expression. The orders' end goes back to order 0. The file marks that order as its loop and plays it
// twice.
void TestNotesAndLevels()
{
    TestCart cart = Cart(Build::k2003);
    std::vector<Row> rows(8);
    rows[0] = {NoteEntry(0, 60, kLooped, 64), NoteEffectEntry(1, 72, kLooped, 32, kPan, kLeft)};
    rows[4] = {{0, kMaskNote, kNoteOff}};
    cart.AddSong({rows}, 125, 6);
    const Rom rom = cart.ToRom();

    SongReport report;
    const seqlib::midi::File midi = Convert(rom, report);
    const std::vector<seqlib::midi::Note> first = ChannelNotes(midi, 0);
    const std::vector<seqlib::midi::Note> second = ChannelNotes(midi, 1);
    const std::vector<seqlib::midi::Text> markers = seqlib::midi::Markers(midi);

    SEQLIB_CHECK_EQ(report.tracks, 2);
    SEQLIB_REQUIRE_EQ(first.size(), size_t(2));
    SEQLIB_CHECK(first[0].key == 60 && first[0].on == 0 && first[0].off == 4 * kRowTicks);
    SEQLIB_CHECK(first[1].key == 60 && first[1].on == 8 * kRowTicks && first[1].off == 12 * kRowTicks);
    SEQLIB_REQUIRE_EQ(second.size(), size_t(2));
    SEQLIB_CHECK(second[0].key == 72 && second[0].on == 0 && second[0].off == 8 * kRowTicks);
    SEQLIB_CHECK(second[1].on == 8 * kRowTicks && second[1].off == 16 * kRowTicks);
    SEQLIB_CHECK(seqlib::midi::Bends(midi).empty());
    SEQLIB_REQUIRE_EQ(markers.size(), size_t(2));
    SEQLIB_CHECK(markers[0].text == "loopStart" && markers[0].tick == 0);
    SEQLIB_CHECK(markers[1].text == "loopEnd" && markers[1].tick == 8 * kRowTicks);
    SEQLIB_CHECK(report.loop_start == 0 && Near(report.loop_end, Seconds(8 * kRow), 1e-6));
    SEQLIB_CHECK(Near(report.seconds, Seconds(16 * kRow), 1e-6));

    // Volume 64 at the music's volume of 4 gives 4 × 64 × 64 >> 10 = 16 of 64 on each side. Volume 32 on the left alone
    // gives 4 × 32 × 128 >> 10 = 16 on the left and 0 on the right.
    int pan = 64, centre = 0, left = 0;
    seqlib::midi::LevelsToControllers(32, 32, pan, centre);
    SEQLIB_CHECK(Sets(midi, 0, 11, centre));
    seqlib::midi::LevelsToControllers(32, 0, pan, left);
    SEQLIB_CHECK(Sets(midi, 1, 10, pan) && Sets(midi, 1, 11, left));
    SEQLIB_CHECK_EQ(kStartMusicVolume, 4);
}

// A jump to order 1 at the end of the last order marks the loop from order 1. Tempo 150 makes order 0's ticks
// 40,000 / 150 = 266 samples, and order 1's tempo of 125 makes them 320. The tempo map follows each row's length.
void TestJumpAndTempo()
{
    TestCart cart = Cart(Build::k2003);
    std::vector<Row> first(4), second(4), third(4);
    first[0] = {NoteEntry(0, 60, kLooped, 64), EffectEntry(1, kTempo, 150)};
    second[0] = {NoteEntry(0, 60, kLooped, 64), EffectEntry(1, kTempo, 125)};
    third[3] = {EffectEntry(0, kJump, 1)};
    cart.AddSong({first, second, third}, 125, 6);
    const Rom rom = cart.ToRom();

    SongReport report;
    const seqlib::midi::File midi = Convert(rom, report);
    const std::vector<seqlib::midi::Text> markers = seqlib::midi::Markers(midi);
    const std::vector<seqlib::midi::Tempo> tempos = seqlib::midi::Tempos(midi);

    const uint32_t fast_row = 6 * (40000 / 150);
    SEQLIB_REQUIRE_EQ(markers.size(), size_t(2));
    SEQLIB_CHECK(markers[0].tick == 4 * kRowTicks && markers[1].tick == 12 * kRowTicks);
    SEQLIB_CHECK(Near(report.loop_start, Seconds(4 * fast_row), 1e-6));
    SEQLIB_CHECK(Near(report.loop_end, Seconds(4 * fast_row + 8 * kRow), 1e-6));
    SEQLIB_REQUIRE_EQ(tempos.size(), size_t(2));
    SEQLIB_CHECK(tempos[0].tick == 0 && tempos[1].tick == 4 * kRowTicks);
    const double expected = Seconds(kRow) * midi.division / kRowTicks * 1e6;
    SEQLIB_CHECK(std::fabs(tempos[1].micros_per_quarter - expected) <= 1);
    SEQLIB_CHECK(std::fabs(double(tempos[1].micros_per_quarter) / tempos[0].micros_per_quarter -
                           double(kRow) / fast_row) < 1e-5);
}

// A loop in which no voice sounds is the wait after a jingle: the song ends where the loop starts, without a loop. The
// sample without a loop ends in the first tick, and its note ends at the next.
void TestSilentLoop()
{
    TestCart cart = Cart(Build::k2003);
    std::vector<Row> first(4), second(4);
    first[0] = {NoteEntry(0, 60, kOneShot, 64)};
    second[3] = {EffectEntry(0, kJump, 1)};
    cart.AddSong({first, second}, 125, 6);
    const Rom rom = cart.ToRom();

    SongReport report;
    const seqlib::midi::File midi = Convert(rom, report);
    const std::vector<seqlib::midi::Note> notes = ChannelNotes(midi, 0);

    SEQLIB_CHECK(report.loop_start < 0 && seqlib::midi::Markers(midi).empty());
    SEQLIB_CHECK(Near(report.seconds, Seconds(4 * kRow), 1e-6));
    SEQLIB_REQUIRE_EQ(notes.size(), size_t(1));
    SEQLIB_CHECK(notes[0].on == 0 && notes[0].off == kRowTicks / 6);
}

// A retrigger every 3 ticks starts the note again halfway through a row of 6 ticks, and a delay of 2 ticks starts its
// note a third of the way through.
void TestRetriggerAndDelay()
{
    TestCart cart = Cart(Build::k2003);
    std::vector<Row> rows(4);
    rows[0] = {NoteEffectEntry(0, 60, kLooped, 64, kRetrigger, 0x03), NoteEffectEntry(1, 60, kLooped, 64, kDelay, 2)};
    cart.AddSong({rows}, 125, 6);
    const Rom rom = cart.ToRom();

    SongReport report;
    const seqlib::midi::File midi = Convert(rom, report);
    const std::vector<seqlib::midi::Note> retriggered = ChannelNotes(midi, 0);
    const std::vector<seqlib::midi::Note> delayed = ChannelNotes(midi, 1);

    SEQLIB_REQUIRE_EQ(retriggered.size(), size_t(4));
    SEQLIB_CHECK(retriggered[0].on == 0 && retriggered[0].off == kRowTicks / 2);
    SEQLIB_CHECK(retriggered[1].on == kRowTicks / 2 && retriggered[1].off == 4 * kRowTicks);
    SEQLIB_REQUIRE_EQ(delayed.size(), size_t(2));
    SEQLIB_CHECK_EQ(delayed[0].on, uint64_t(kRowTicks / 3));
}

// An offset of 2 starts the note 512 points into its sample: a preset of its own, whose zone starts there, on the
// SoundFont sample that a note from the start plays too.
void TestOffset()
{
    TestCart cart = Cart(Build::k2003);
    std::vector<Row> rows(4);
    rows[0] = {NoteEffectEntry(0, 60, kLooped, 64, kOffset, 2), NoteEntry(1, 60, kLooped, 64)};
    cart.AddSong({rows}, 125, 6);
    const Rom rom = cart.ToRom();

    InstrumentSet shared;
    const SongReport report = ConvertSong(rom, Detect(rom), 0, TempSettings("webfoot_offset"), &shared);
    const Sf2File sf = shared.Build(rom);

    SEQLIB_REQUIRE(report.ok && report.tracks == 2);
    SEQLIB_CHECK_EQ(sf.samples.size(), size_t(1));
    SEQLIB_REQUIRE_EQ(sf.presets.size(), size_t(2));
    SEQLIB_CHECK_EQ(CountGenerators(sf, seqlib::sf2::kStartAddrsOffset, 512), 1);
}

// A ping-pong loop becomes the sample's points to its end, then the loop's points backwards, looped over both. The
// mixer turns at the edges of the loop's first and last points, and each of them plays on both sides of the turn.
void TestPingPongSample()
{
    const TestCart cart = Cart(Build::k2003);
    const Rom rom = cart.ToRom();
    const uint32_t entry = kSamples + kSampleSize * kPingPong;
    const Sample s = ReadSample(rom, entry);
    PlayedSample played;
    played.entry = entry;
    played.points = s.points;
    played.length = s.length;
    played.loop_start = s.loop_start;
    played.flags = s.flags;
    played.rate = s.rate;

    InstrumentSet set;
    set.Program(played, kPingPong);
    const Sf2File sf = set.Build(rom);

    SEQLIB_REQUIRE_EQ(sf.samples.size(), size_t(1));
    const seqlib::sf2::Sample& out = sf.samples[0];
    SEQLIB_CHECK_EQ(out.loop_start, uint32_t(500));
    SEQLIB_CHECK_EQ(out.loop_end, uint32_t(1500));
    SEQLIB_REQUIRE(out.data.size() >= 1500);
    bool forward = true, backward = true;
    for (uint32_t i = 0; i < 1000; i++)
    {
        forward = forward && out.data[i] == rom.S8(s.points + i) * 256;
    }

    for (uint32_t i = 0; i < 500; i++)
    {
        backward = backward && out.data[1000 + i] == rom.S8(s.points + 999 - i) * 256;
    }

    SEQLIB_CHECK(forward && backward);
}

// The voice's step sets the pitch of a ping-pong loop's note in both builds. The 2003 build turns a ping-pong loop as a
// reflection. The 2002 build turns at the loop's start to a place below it. That changes the points that play but not
// the step. Each note keeps the key of its pitch without a bend.
void TestPingPongPitch()
{
    for (const Build build : {Build::k2003, Build::k2002})
    {
        TestCart cart = Cart(build);
        std::vector<Row> rows(32);
        rows[0] = {NoteEntry(0, 60, kPingPong, 64)};
        cart.AddSong({rows}, 125, 6);
        const Rom rom = cart.ToRom();

        SongReport report;
        const seqlib::midi::File midi = Convert(rom, report);
        const std::vector<seqlib::midi::Note> notes = ChannelNotes(midi, 0);
        const std::vector<seqlib::midi::Bend> bends = seqlib::midi::Bends(midi);

        SEQLIB_REQUIRE(!notes.empty());
        SEQLIB_CHECK(notes[0].key == 60 && bends.empty());
    }
}

int RunTests()
{
    TestNotesAndLevels();
    TestJumpAndTempo();
    TestSilentLoop();
    TestRetriggerAndDelay();
    TestOffset();
    TestPingPongSample();
    TestPingPongPitch();

    return seqlib::testing::Summary("webfoot_conversion");
}

} // namespace
} // namespace agblib::sequence_export::webfoot

int main()
{
    return agblib::sequence_export::webfoot::RunTests();
}
