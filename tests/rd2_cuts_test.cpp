// SPDX-License-Identifier: MIT

// Unit tests for the cuts of Nintendo R&D2's driver and its free voices: a track that ends a sound in another track,
// and the All Sound Off events of the conversion. They build small synthetic cartridges in memory. No game ROM is
// needed.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/controllers.h"
#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/rd2/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/rd2/convert.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/rd2_cart.h"
#include "rd2_test_util.h"

namespace agblib::sequence_export::rd2
{
namespace
{

using namespace agblib::testing::rd2_cart;

namespace cc = seqlib::midi::cc;
using agblib::agb::Rom;
using agblib::rd2::DriverInfo;
using agblib::rd2::Event;
using agblib::sequence_export::agb::ConvertSettings;
using agblib::sequence_export::agb::SongReport;
using agblib::testing::agb_files::MidiEvents;
using agblib::testing::agb_files::ReadMidi;
using agblib::testing::agb_files::TempSettings;
using agblib::testing::rd2_tests::Detect;
using agblib::testing::rd2_tests::Run;

// Returns a track's commands, without the end that TrackData::End() adds.
std::vector<uint8_t> Commands(TrackData t)
{
    std::vector<uint8_t> bytes = t.End();
    bytes.pop_back();

    return bytes;
}

// Returns the tracks of a sequence: each plays its `intro`, then its `loop` again and again through F0. A track without
// a loop ends after its intro.
std::vector<std::vector<uint8_t>> LoopingTracks(
    const std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>& tracks)
{
    uint32_t at = 2 + 2 * uint32_t(tracks.size()); // the sequence's header
    std::vector<std::vector<uint8_t>> out;
    for (const auto& [intro, loop] : tracks)
    {
        std::vector<uint8_t> t = intro;
        const uint32_t target = at + uint32_t(intro.size());
        t.insert(t.end(), loop.begin(), loop.end());
        if (!loop.empty())
        {
            t.insert(t.end(), {0xF0, uint8_t(target), uint8_t(target >> 8)});
        }

        t.push_back(0xFF);
        at += uint32_t(t.size());
        out.push_back(t);
    }

    return out;
}

// Converts sequence 0 with `loops` passes through its loop, and returns the MIDI file.
MidiEvents ConvertCuts(const Rom& rom, const std::string& name, int loops = 2)
{
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings(name);
    opt.loops = loops;
    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);
    SEQLIB_CHECK(sum.ok);

    return sum.ok ? ReadMidi(sum.midi_path) : MidiEvents();
}

// Returns the All Sound Off events (CC120) of a MIDI file, as (channel, tick), in the order of their ticks.
std::vector<std::pair<int, uint64_t>> SoundOffs(const MidiEvents& midi)
{
    std::vector<std::pair<int, uint64_t>> offs;
    for (int c = 0; c < 16; c++)
    {
        for (const auto& e : midi.Find(uint8_t(0xB0 + c)))
        {
            if (e.bytes[1] == cc::kAllSoundOff)
            {
                offs.emplace_back(c, e.tick);
            }
        }
    }

    std::sort(offs.begin(), offs.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

    return offs;
}

// Returns true if channel `c` has a note-on at `tick` after an All Sound Off at that tick.
bool NoteAfterSoundOff(const MidiEvents& midi, int c, uint64_t tick)
{
    for (const auto& track : midi.tracks)
    {
        bool off = false;
        for (const auto& e : track.events)
        {
            if (e.tick != tick || e.bytes.size() < 3)
            {
                continue;
            }

            off = off || (e.bytes[0] == 0xB0 + c && e.bytes[1] == cc::kAllSoundOff);
            if (e.bytes[0] == 0x90 + c && e.bytes[2] > 0)
            {
                return off;
            }
        }
    }

    return false;
}

// The cuts of sequences at a tempo of 150, a tick a frame. Track 0 plays `cutters` notes of the plain sample at tick 4.
// Track 1 plays its notes from tick 0 in `track1`. The mixer has 7 voices, and a note takes a free voice first, then
// the oldest released voice, then the oldest that plays.
Rom CutRom(int cutters, const TrackData& track1, const std::vector<uint8_t>& track2 = {})
{
    TrackData cutter;
    cutter.Bytes({0xE4, 0x80, 150}).Wait(4).Bytes({0xC2, kPlain});
    for (int n = 0; n < cutters; n++)
    {
        cutter.Note(uint8_t(70 + n), 10, 100);
    }

    std::vector<std::vector<uint8_t>> tracks = {cutter.Wait(10).End(), TrackData(track1).End()};
    if (!track2.empty())
    {
        tracks.push_back(track2);
    }

    Cart cart;
    cart.Sequences({tracks});

    return cart.ToRom();
}

void TestCutSoundOff()
{
    // Track 1's note releases at tick 1 and fades. At tick 4, track 0's seventh note takes its voice. Nothing else of
    // track 1 sounds. Its channel gets All Sound Off at tick 4 (600 of the MIDI file's ticks). Its square note of that
    // tick starts after it.
    TrackData released;
    released.Bytes({0xC2, kLooped}).Note(60, 1, 127).Wait(4).Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(4);

    const MidiEvents cut = ConvertCuts(CutRom(7, released), "rd2_cut");
    const MidiEvents none = ConvertCuts(CutRom(6, released), "rd2_no_cut");

    const std::vector<std::pair<int, uint64_t>> kOne = {{1, 600}};
    SEQLIB_CHECK(SoundOffs(cut) == kOne);
    SEQLIB_CHECK(NoteAfterSoundOff(cut, 1, 600));
    SEQLIB_CHECK(SoundOffs(none).empty());

    // At a tempo of 300, a frame plays two ticks. Track 1's square note at tick 9 and the cut at tick 10 fall in one
    // frame. All Sound Off comes at tick 9 (1350), before the note, rather than at the cut, where it would stop the
    // note.
    TrackData early;
    early.Bytes({0xE4, 0x81, 0x2C}).Wait(10).Bytes({0xC2, kPlain});
    for (int n = 0; n < 7; n++)
    {
        early.Note(uint8_t(70 + n), 2, 100);
    }

    TrackData square;
    square.Bytes({0xC2, kLooped}).Note(60, 2, 127).Wait(9).Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(4);
    Cart cart;
    cart.Sequences({{early.Wait(10).End(), square.End()}});
    const MidiEvents before = ConvertCuts(cart.ToRom(), "rd2_cut_early");

    const std::vector<std::pair<int, uint64_t>> kEarly = {{1, 1350}};
    SEQLIB_CHECK(SoundOffs(before) == kEarly);
    SEQLIB_CHECK(NoteAfterSoundOff(before, 1, 1350));
}

void TestCutWithHeldNote()
{
    // Track 1 holds a second note. Track 0's sixth note takes the released note's voice. The held note sounds on, and
    // All Sound Off would stop it. The file has none.
    TrackData held;
    held.Bytes({0xC2, kLooped}).Note(60, 1, 127).Note(62, 20, 127).Wait(20);

    SEQLIB_CHECK(SoundOffs(ConvertCuts(CutRom(6, held), "rd2_cut_held")).empty());
}

void TestCutOfPlayingNote()
{
    // Track 0's seventh note takes the voice of track 1's note while it plays. The note off and All Sound Off both
    // come at the cut, tick 4.
    TrackData playing;
    playing.Bytes({0xC2, kLooped}).Note(60, 20, 127).Wait(20);

    const MidiEvents midi = ConvertCuts(CutRom(7, playing), "rd2_cut_playing");

    const std::vector<std::pair<int, uint64_t>> kOne = {{1, 600}};
    SEQLIB_CHECK(SoundOffs(midi) == kOne);
    std::vector<uint64_t> offs;
    for (const auto& e : midi.Find(0x81))
    {
        offs.push_back(e.tick);
    }

    for (const auto& e : midi.Find(0x91))
    {
        if (e.bytes[2] == 0)
        {
            offs.push_back(e.tick);
        }
    }

    SEQLIB_CHECK(offs == std::vector<uint64_t>{600});
}

void TestCutWithPsgNote()
{
    // Track 1's square note sounds across the cut of its released note. All Sound Off would stop it. The file has none.
    TrackData psg;
    psg.Bytes({0xC2, kSquare}).Note(0x30, 20, 127).Bytes({0xC2, kLooped}).Note(60, 1, 127).Wait(20);

    SEQLIB_CHECK(SoundOffs(ConvertCuts(CutRom(7, psg), "rd2_cut_psg")).empty());
}

void TestCutWithMissingRegion()
{
    // The drum kit has no region for note 35, below its first. The driver plays such a note from past its tables. The
    // model's voices can then differ from the game's. The sequence gets no All Sound Off.
    TrackData released;
    released.Bytes({0xC2, kLooped}).Note(60, 1, 127).Wait(4).Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(4);
    const Rom rom = CutRom(7, released, TrackData().Wait(8).Bytes({0xC2, kDrums}).Note(35, 1, 127).End());
    const DriverInfo info = Detect(rom);
    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_cut_missing"), nullptr);

    SEQLIB_REQUIRE(sum.ok);
    SEQLIB_CHECK(std::any_of(sum.warnings.begin(), sum.warnings.end(),
                             [](const std::string& w) { return w.find("has no region") != std::string::npos; }));
    SEQLIB_CHECK(SoundOffs(ReadMidi(sum.midi_path)).empty());

    // The same holds where the file leaves out the track with that note.
    ConvertSettings masked = TempSettings("rd2_cut_missing_masked");
    masked.track_mask = 0x3;
    const SongReport two = ConvertSong(rom, info, 0, masked, nullptr);
    SEQLIB_REQUIRE(two.ok);
    SEQLIB_CHECK(SoundOffs(ReadMidi(two.midi_path)).empty());
}

void TestCutsInLoop()
{
    // Five tracks at a tick a frame. Track 2 has no loop: its note releases at tick 2, where the loop starts. The other
    // tracks loop every 40 ticks. Track 1's note releases at tick 1. At tick 4, track 0's six notes take the five free
    // voices and the oldest released one, track 1's. The next pass has no note of track 2, and its six notes find six
    // free voices. A looping player would play the cut's All Sound Off where the game cuts nothing. The marked pass
    // therefore has none. At tick 29 of each pass, track 4's seven notes cut track 3's released note. That one repeats,
    // and the marked pass keeps its All Sound Off on channel 3, at tick 29 (4350).
    TrackData six;
    six.Wait(4).Bytes({0xC2, kPlain});
    for (int n = 0; n < 6; n++)
    {
        six.Note(uint8_t(70 + n), 1, 100);
    }

    TrackData seven;
    seven.Wait(29).Bytes({0xC2, kPlain});
    for (int n = 0; n < 7; n++)
    {
        seven.Note(uint8_t(70 + n), 1, 100);
    }

    const std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> tracks = {
        {Commands(TrackData().Bytes({0xE4, 0x80, 150})), Commands(six.Wait(36))},
        {{}, Commands(TrackData().Bytes({0xC2, kLooped}).Note(62, 1, 127).Wait(40))},
        {Commands(TrackData().Bytes({0xC2, kLooped}).Note(60, 2, 127).Wait(2)), {}},
        {{}, Commands(TrackData().Wait(25).Bytes({0xC2, kLooped}).Note(64, 1, 127).Wait(15))},
        {{}, Commands(seven.Wait(11))},
    };
    Cart cart;
    cart.Sequences({LoopingTracks(tracks)});
    const Rom rom = cart.ToRom();

    const MidiEvents two = ConvertCuts(rom, "rd2_cut_loop2", 2);
    const MidiEvents one = ConvertCuts(rom, "rd2_cut_loop1", 1);

    // The loop runs from tick 2 to tick 42, 300 to 6300 of the MIDI file's.
    const auto marked = [](const MidiEvents& midi)
    {
        std::vector<std::pair<int, uint64_t>> in;
        for (const auto& off : SoundOffs(midi))
        {
            if (off.second >= 300 && off.second < 6300)
            {
                in.push_back(off);
            }
        }

        return in;
    };
    const std::vector<std::pair<int, uint64_t>> kMarked = {{3, 4350}};
    const std::vector<std::pair<int, uint64_t>> kAll = {{3, 4350}, {3, 10350}};
    SEQLIB_CHECK(marked(two) == kMarked);
    SEQLIB_CHECK(SoundOffs(two) == kAll);
    SEQLIB_CHECK(marked(one) == kMarked);
}

void TestFreeVoices()
{
    // With the free list that Super Mario Advance 4's boot leaves, voice 1 comes first, then voice 0.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kLooped}).Note(60, 20, 100).Note(62, 20, 100).Wait(20).End()}});
    DriverInfo info = Detect(cart.ToRom());
    SEQLIB_CHECK((info.free_voices == std::array<uint8_t, 7>{0, 1, 2, 3, 4, 5, 6}));
    info.free_voices = {1, 0, 2, 3, 4, 5, 6};

    std::vector<int> voices;
    for (const auto& frame : Run(cart.ToRom(), info, 0, 1))
    {
        for (const Event& e : frame)
        {
            if (e.kind == Event::kNote)
            {
                voices.push_back(e.voice);
            }
        }
    }

    SEQLIB_CHECK((voices == std::vector<int>{1, 0}));

    // Detection sets that order for the game's revision 1, and only there.
    cart.SetGameCode("AX4E");

    SEQLIB_CHECK((Detect(cart.ToRom()).free_voices == std::array<uint8_t, 7>{0, 1, 2, 3, 4, 5, 6}));
    cart.Put8(agblib::agb::kRomBase + 0xBC, 1);
    SEQLIB_CHECK((Detect(cart.ToRom()).free_voices == std::array<uint8_t, 7>{1, 0, 2, 3, 4, 5, 6}));
}

} // namespace

int RunTests()
{
    TestCutSoundOff();
    TestCutWithHeldNote();
    TestCutOfPlayingNote();
    TestCutWithPsgNote();
    TestCutWithMissingRegion();
    TestCutsInLoop();
    TestFreeVoices();

    return seqlib::testing::Summary("rd2_cuts");
}

} // namespace agblib::sequence_export::rd2

int main()
{
    return agblib::sequence_export::rd2::RunTests();
}
