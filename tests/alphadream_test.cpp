// SPDX-License-Identifier: MIT

// Unit tests for AlphaDream's driver. They build a small synthetic cartridge in memory. No game ROM is needed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include "seqlib/sf2/soundfont.h"
#include "seqlib/testing/check.h"

#include "agblib/agb/files.h"
#include "agblib/agb/rom.h"
#include "agblib/alphadream/commands.h"
#include "agblib/alphadream/driver.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/alphadream/convert.h"
#include "agblib/sequence_export/alphadream/performance.h"
#include "agblib/sequence_export/alphadream/sequencer.h"
#include "agblib/sequence_export/alphadream/soundfont.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/agb_files.h"
#include "alphadream_test_cart.h"

namespace agblib::sequence_export::alphadream
{

namespace sf2 = seqlib::sf2;

using agblib::agb::Utf8;
using agblib::alphadream::SampleHeader;
using agblib::alphadream::Version;
using agblib::sequence_export::agb::MelodicChannel;

namespace
{

using namespace agblib::testing::alphadream_test_cart;
using agblib::testing::agb_files::MidiEvents;
using agblib::testing::agb_files::ReadAll;
using agblib::testing::agb_files::ReadMidi;
using agblib::testing::agb_files::ReadSf2;
using agblib::testing::agb_files::Sf2Records;
using agblib::testing::agb_files::TempPath;
using agblib::testing::agb_files::TempSettings;

// Detection finds each table through its routine's literal.
void TestDetection()
{
    const Rom rom = TestCartridge({0xFF}).Build();
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, info, error));

    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(info.song_table == kSongTable);
    SEQLIB_CHECK(info.song_count == 2);
    SEQLIB_CHECK(info.instrument_table == kInstruments);
    SEQLIB_CHECK(info.instrument_count == 2);
    SEQLIB_CHECK(info.sample_table == kSamples);
    SEQLIB_CHECK(info.sample_count == 1);
    SEQLIB_CHECK(info.step_table == kStep + 0x38);
    SEQLIB_CHECK(info.key_table == kStep + 0x6C);
    SEQLIB_CHECK(info.psg_pitch_table == kPsgPitchTable);
    SEQLIB_CHECK(info.noise_table == kNoiseTable);
    SEQLIB_CHECK(info.wave_table == kWaveTable);
    SEQLIB_CHECK(info.wave_levels == kWaveLevels);

    Song song;

    SEQLIB_CHECK(!ReadSong(rom, info, 0, song));
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));

    SEQLIB_CHECK(song.mask == 0x104);
    SEQLIB_CHECK(song.tracks[2] == kSong + 0x20 && song.tracks[8] == kSong + 0x90);

    Region r;

    SEQLIB_CHECK(FindRegion(rom, info, 0, 60, r) && r.sample == 0 && r.envelope[2] == 0x80);
    SEQLIB_CHECK(!FindRegion(rom, info, 0, 30, r));

    Rom empty;
    empty.Assign(std::vector<uint8_t>(0x1000, 0));

    SEQLIB_CHECK(!DetectDriver(empty, info, error) && error.empty());

    // The Hamtaro games' routine that starts a song gives the same tables.
    const Rom hamtaro = TestCartridge({0xFF}, kHamtaroStartCode, 0x2E).Build();

    SEQLIB_CHECK(DetectDriver(hamtaro, info, error));

    SEQLIB_CHECK(info.start_routine == kStart && info.song_table == kSongTable && info.song_count == 2);
}

// The start of a note gives the driver's version, and the table of sound effects ends the song table.
void TestVersions()
{
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(DetectDriver(TestCartridge({0xFF}).Build(), info, error));

    SEQLIB_CHECK(info.version == Version::kSuperstarSaga && info.effect_table == 0 && info.song_count == 2);

    Cartridge sports = TestCartridge({0xFF});
    sports.Code(kNote, kHamSportsNoteCode);
    SEQLIB_CHECK(DetectDriver(sports.Build(), info, error) && info.version == Version::kHamSports);

    Cartridge older = TestCartridge({0xFF});
    older.Code(kNote, kHamtaro4NoteCode);
    SEQLIB_CHECK(DetectDriver(older.Build(), info, error) && info.version == Version::kHamtaro4);

    // The table of sound effects follows the song table's first entry. Song 1 is therefore an effect.
    Cartridge effects = TestCartridge({0xFF});
    effects.Routine(kCommands, kCommandCode, 8, kCommandTable);
    effects.U32(kCommandTable + 4 * 5, kEffectHandler | 1);
    effects.Code(kEffectHandler, kEffectHandlerCode);
    effects.Bl(kEffectHandler + 6, kEffectStart);
    effects.Routine(kEffectStart, kEffectStartCode, 2, kSongTable + 4);

    SEQLIB_CHECK(DetectDriver(effects.Build(), info, error));

    SEQLIB_CHECK(info.effect_table == kSongTable + 4 && info.song_count == 1);
}

// B85A's and A84J's notes have a third byte, the track's volume. For a tied note, B85A's driver keeps the volume for
// later, and A84J's driver works the levels out at once. F1 is a note's length in both. In B85A, FA keeps a priority.
void TestHamtaroNotes()
{
    DriverInfo info;
    std::string error;
    Song song;

    // Pan 0x40, then a note at volume 0x80 tied to a note of 0xF1 ticks at volume 0x20.
    Cartridge sports = TestCartridge({0xF9, 0x4B, 0xF2, 0x40, 0xFA, 0x03, 0x00, 0x3C, 0x80, 0xF1, 0x3E, 0x20, 0xFF});
    sports.Code(kNote, kHamSportsNoteCode);
    const Rom b85a = sports.Build();
    SEQLIB_CHECK(DetectDriver(b85a, info, error) && ReadSong(b85a, info, 1, song));
    Sequencer b(b85a, info, song);
    SEQLIB_CHECK(b.Track(2).flags == (flag::kMuted | flag::kUsed));

    b.Step();

    SEQLIB_CHECK(b.Track(2).wait == 0xF1 && b.Track(2).volume == 0x20 && b.Channel(2).pitch == 0x3E * 256);
    // The first note's volume: the right side at 129 × 127 / 256 and the left at 129 × 64 / 256.
    SEQLIB_CHECK(b.Channel(2).right == 63 && b.Channel(2).left == 32);

    Cartridge older = TestCartridge({0xF9, 0x4B, 0xF2, 0x40, 0x00, 0x3C, 0x80, 0xF1, 0x3E, 0x20, 0xFF});
    older.Code(kNote, kHamtaro4NoteCode);
    const Rom a84j = older.Build();
    SEQLIB_CHECK(DetectDriver(a84j, info, error) && ReadSong(a84j, info, 1, song));
    Sequencer a(a84j, info, song);

    a.Step();

    SEQLIB_CHECK(a.Track(2).wait == 0xF1 && a.Channel(2).pitch == 0x3E * 256);
    // The tied note's volume: the right side at 33 × 127 / 256 and the left at 33 × 64 / 256.
    SEQLIB_CHECK(a.Channel(2).right == 16 && a.Channel(2).left == 8);
}

// A note's timing, levels and envelope. At tempo 75 and the divisor 0x4B, the count of 45 that the first frame leaves
// passes the divisor each frame. A tick therefore runs on each frame.
void TestSequencer()
{
    const Rom rom = TestCartridge({0xF9, 0x4B, 0xF1, 0x80, 0xF2, 0x40, 0x08, 0x3C, 0xF6, 0x02, 0xFF}).Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);

    // Frame 0: the restore and the first note-on with key 0. No region contains that key. The tempo is still 0x78, and
    // its count of 120 passes 75: a tick runs, the wait of 1 ends, and the track reads its tempo, volume, pan and note.
    seq.Step();

    SEQLIB_CHECK(seq.Track(2).tempo == 0x4B);
    SEQLIB_CHECK(seq.Track(2).key == 60);
    SEQLIB_CHECK(seq.Track(2).wait == 8);
    const SampleChannel& c = seq.Channel(2);
    SEQLIB_CHECK(c.status == 0x81);
    SEQLIB_CHECK(c.level == 0x40);
    SEQLIB_CHECK(c.pitch == 60 * 256);
    // Volume 0x80 and pan 0x40: the right side at 129 × 127 / 256 and the left at 129 × 64 / 256.
    SEQLIB_CHECK(c.right == 63 && c.left == 32);

    // The attack rises by 0x40 a frame to 255, then the level drops to the sustain level.
    seq.Step();
    SEQLIB_CHECK(c.level == 0x80 && c.status == 0x81);
    seq.Step();
    SEQLIB_CHECK(c.level == 0xC0);
    seq.Step();
    SEQLIB_CHECK(c.level == 0xFF && c.status == 0x82);
    seq.Step();
    SEQLIB_CHECK(c.level == 0x80 && c.status == 0x83);

    // The note's 8 ticks end on frame 8. The release takes 0x20 a frame off the sustain level of 0x80, and the
    // channel stops when the level would reach 0.
    for (int i = 0; i < 4; i++)
    {
        seq.Step();
    }

    SEQLIB_CHECK(c.status == 0xC3 && c.level == 0x60);
    seq.Step();
    seq.Step();
    SEQLIB_CHECK(c.status == 0xC3 && c.level == 0x20);
    seq.Step();
    SEQLIB_CHECK(c.status == 0);
    SEQLIB_CHECK(!(seq.Track(2).flags & flag::kUsed));

    // Track 8's square note comes after track 2's, on the same frame.
    SEQLIB_CHECK(seq.Notes().size() == 2);
    const Note& n = seq.Notes().front();
    SEQLIB_CHECK(n.track == 2 && n.key == 60 && n.on == 0 && n.off == 8);
    SEQLIB_CHECK(seq.Notes()[1].track == 8 && seq.Notes()[1].key == 0x48 && seq.Notes()[1].instrument == 1);
}

// The rests' state. A note that isn't tied, with bit 7 in its key, sets it. From then on, a rest follows each note that
// isn't tied, where the note ends (0x0819B4C0). The F6 rest, the tied note's wait, the rest after a note and the F7 end
// without one. At tempo 75, a tick passes each frame.
void TestRests()
{
    const Rom rom = TestCartridge({0xF9, 0x4B, 0xF0, 0x00, 0xF1, 0xC0, 0x04, 0xBC, 0x03, 0xF6, 0x05,
                                   0x00, 0xBE, 0x02, 0x02, 0x40, 0x01, 0xF7, 0x03, 0x3C, 0x02, 0xFF})
                        .Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);

    std::vector<int> waits;
    std::vector<int> keys;
    for (int f = 0; f < 22; f++)
    {
        seq.Step();
        waits.push_back(seq.Track(2).wait);
        keys.push_back(seq.Track(2).key);
    }

    const bool playing = seq.Track(2).flags & flag::kUsed;
    seq.Step();

    // The note of 4 ticks ends on frame 4, and its rest of 3 on frame 7. The F6 rest of 5 runs to frame 12. The tied
    // note waits 2 ticks, and the note of key 64 then goes on from it for 2 more. Its rest of 1 ends on frame 17, where
    // the F7 finds no note and the last note starts. Its rest of 2 ends on frame 22, at the track's end.
    const std::vector<int> kExpectedWaits = {4, 3, 2, 1, 3, 2, 1, 5, 4, 3, 2, 1, 2, 1, 2, 1, 1, 3, 2, 1, 2, 1};
    SEQLIB_CHECK(waits == kExpectedWaits);
    SEQLIB_CHECK(keys[0] == 60 && keys[12] == 62 && keys[14] == 64 && keys[17] == 60);
    SEQLIB_CHECK(playing && !(seq.Track(2).flags & flag::kUsed));
    SEQLIB_CHECK(seq.Notes().size() >= 3);
    const Note& first = seq.Notes().front();
    const Note& last = seq.Notes().back();
    SEQLIB_CHECK(first.key == 60 && first.on == 0 && first.released == 4);
    SEQLIB_CHECK(last.key == 60 && last.on == 17 && last.released == 20);
}

// A square's envelope, as the driver works the hardware's envelope (0x0819AC84) and the PSG plays it. Square 1 plays
// key 0x48 for 8 ticks at tempo 75 and volume 0x80, with the envelope bytes C8 FF FF C8 that most of A88E's rising
// squares have: a rise from 0 and a fall to 0, each at a step of 1.
void TestPsgEnvelope()
{
    Cartridge c = TestCartridge({0xFF});
    c.Bytes(kInstruments + 18, {0xC8, 0xFF, 0xFF, 0xC8});
    c.Bytes(kSong + 0x90, {0xF9, 0x4B, 0xF0, 0x01, 0xF1, 0x80, 0xF2, 0x7F, 0x08, 0x48, 0xFF});
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);
    const PsgChannel& p = seq.Psg(0);

    // Frame 0: the note starts the rise at volume 0 and a step of 1 (0x09). The channel sounds from volume 0.
    seq.Step();

    SEQLIB_CHECK(p.volume == 8 && p.state == 2 && p.written == 0x09 && p.count == 0);
    SEQLIB_CHECK(seq.PsgVolume(0) == 0);

    // The driver counts a step a frame. The hardware takes 64 steps a second.
    for (int f = 1; f < 8; f++)
    {
        seq.Step();
        SEQLIB_CHECK(p.state == 2 && p.count == f);
        SEQLIB_CHECK(seq.PsgVolume(0) == f);
    }

    // Frame 8: the note ends before the rise does, at the driver's volume of 8. The fall starts from it (0x81).
    seq.Step();

    SEQLIB_CHECK(p.state == 6 && p.written == 0x81 && p.count == 0);
    SEQLIB_CHECK(seq.PsgVolume(0) == 8);

    // The hardware reaches 0 on frame 15. The driver silences the channel on frame 16, once its count reaches 8.
    const int kFalls[] = {7, 6, 5, 4, 2, 1, 0};
    for (int v : kFalls)
    {
        seq.Step();
        SEQLIB_CHECK(seq.PsgVolume(0) == v);
    }

    SEQLIB_CHECK(p.state == 6);
    seq.Step();
    SEQLIB_CHECK(p.state == 0 && p.written == 0 && seq.PsgVolume(0) == -1);
    SEQLIB_CHECK(!seq.Playing());

    // The note lasts while the channel sounds. Each frame's level is the mean of the hardware's volumes at the frame's
    // start and end. Frames 7 and 8 have the same level, and the changes keep frame 7's alone.
    SEQLIB_CHECK(seq.Notes().size() == 1);
    const Note& n = seq.Notes().front();
    SEQLIB_CHECK(n.track == 8 && n.on == 0 && n.off == 15);
    // The track releases the note on frame 8. The beat uses that frame.
    SEQLIB_CHECK(n.released == 8);

    std::vector<double> levels;
    for (const Change& ch : seq.Changes())
    {
        levels.push_back(ch.left * 15 / 63);
    }

    const std::vector<double> kExpected = {0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 6.5, 5.5, 4.5, 3, 1.5, 0.5};
    SEQLIB_CHECK(levels.size() == kExpected.size());
    for (size_t i = 0; i < levels.size() && i < kExpected.size(); i++)
    {
        SEQLIB_CHECK(std::abs(levels[i] - kExpected[i]) < 1e-9);
    }
}

// The PSG channels' records on a song's first frames. In each version, the restore of a PSG channel uses the squares'
// pitch routine, for the noise too (0x0819ABF8 in A88E). With key 0, the noise's setting stays 0. A84J's driver counts
// the frames since the last setting even when it has written none (0x0814A844). A88E's driver counts them only after a
// setting.
void TestPsgRecords()
{
    DriverInfo info;
    std::string error;
    Song song;

    // Song 1 also plays track 11, the noise. That track ends at once. The offsets count from the song's start.
    Cartridge sports = TestCartridge({0xFF});
    sports.Code(kNote, kHamSportsNoteCode);
    sports.U16(kSong + 0x10, 0x904);
    sports.U16(kSong + 0x16, 0xA0);
    sports.U8(kSong + 0x10 + 0xA0, 0xFF);
    const Rom b85a = sports.Build();
    SEQLIB_CHECK(DetectDriver(b85a, info, error) && ReadSong(b85a, info, 1, song));
    Sequencer b(b85a, info, song);

    b.Step();

    SEQLIB_CHECK(b.Psg(3).frequency == 0);
    SEQLIB_CHECK(b.Psg(1).count == 0);

    Cartridge older = TestCartridge({0xFF});
    older.Code(kNote, kHamtaro4NoteCode);
    const Rom a84j = older.Build();
    SEQLIB_CHECK(DetectDriver(a84j, info, error) && ReadSong(a84j, info, 1, song));
    Sequencer a(a84j, info, song);

    for (int f = 1; f <= 3; f++)
    {
        a.Step();
        SEQLIB_CHECK(a.Psg(1).count == f && a.Psg(1).written == 0);
    }
}

// The wave (0x0819AF18). Track 10 plays key 72 for 4 ticks at volume 94, then again at volume 200, at tempo 75. A
// note's start sets NR32 by the channel's volume over 4: 5 gives 50% and 12 gives 100%. The note's end mutes the wave.
// The wave's sample plays an octave below a square's at the same key.
void TestWave()
{
    Cartridge c = TestCartridge({0xFF});
    c.U16(kSong + 0x10, 0x404); // tracks 2 and 10
    c.Bytes(kSong + 0x90, {0xF9, 0x4B, 0xF0, 0x01, 0xF1, 0x5E, 0x04, 0x48, 0xF1, 0xC8, 0x04, 0x48, 0xFF});
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);
    const PsgChannel& p = seq.Psg(2);

    seq.Step();
    SEQLIB_CHECK(p.volume == 5 && p.state == 2 && p.duty == 0);
    for (int f = 1; f < 4; f++)
    {
        seq.Step();
        SEQLIB_CHECK(p.state == 2);
    }

    // Frame 4: the second note restarts the wave without a frame of silence.
    seq.Step();
    SEQLIB_CHECK(p.volume == 12 && p.state == 2);
    for (int f = 5; f < 9; f++)
    {
        seq.Step();
    }

    SEQLIB_CHECK(p.state == 0);

    SEQLIB_REQUIRE_EQ(seq.Notes().size(), 2);
    SEQLIB_CHECK(seq.Notes()[0].track == 10 && seq.Notes()[0].on == 0 && seq.Notes()[0].off == 4);
    SEQLIB_CHECK(seq.Notes()[1].track == 10 && seq.Notes()[1].on == 4 && seq.Notes()[1].off == 8);

    std::vector<double> levels;
    for (const Change& ch : seq.Changes())
    {
        levels.push_back(ch.left * 15 / 63);
    }

    SEQLIB_REQUIRE_EQ(levels.size(), 2);
    SEQLIB_CHECK(std::abs(levels[0] - 7.5) < 1e-9 && std::abs(levels[1] - 15) < 1e-9);

    // The wave's preset: a cycle of 32 points at middle C on key 72, with an instant release.
    InstrumentSet set;
    Region r;
    SEQLIB_CHECK(FindRegion(rom, info, 1, 72, r));
    set.Add(Kind::kWave, 1, 72, r);
    Sf2File file;

    set.Build(rom, info, file);

    SEQLIB_REQUIRE_EQ(file.instruments.size(), 1);

    int waves = 0;
    for (const sf2::Sample& s : file.samples)
    {
        if (s.name == "Wave 2")
        {
            waves++;
            SEQLIB_CHECK(s.original_key == 72);
            SEQLIB_CHECK(std::abs(double(s.sample_rate) - 32 * 261.6256) < 1);
        }
    }

    SEQLIB_CHECK(waves == 1);

    for (const sf2::Zone& z : file.instruments[0].zones)
    {
        for (const sf2::Generator& g : z.generators)
        {
            SEQLIB_CHECK(g.oper != sf2::kReleaseVolEnv);
        }
    }
}

// A square plays the frequency setting that the driver writes from its table, 131072 / (2048 - setting) Hz, and the
// setting 0 below key 36 (0x0819A96C). The test's table rounds 131072 over each key's frequency. Key 72 gets 250, and
// key 24 gets 0. Setting 0 plays 64 Hz.
void TestPsgPitch()
{
    Cartridge c = TestCartridge({0xFF});
    c.Bytes(kSong + 0x90, {0xF9, 0x4B, 0xF0, 0x01, 0xF1, 0x80, 0x04, 0x48, 0x04, 0x18, 0xFF});
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);

    for (int f = 0; f < 8; f++)
    {
        seq.Step();
        if (f == 0)
        {
            SEQLIB_CHECK(seq.Psg(0).frequency == 2048 - 250);
        }

        if (f == 4)
        {
            SEQLIB_CHECK(seq.Psg(0).frequency == 0);
        }
    }

    double first = 1000;
    double second = 1000;
    for (const Change& ch : seq.Changes())
    {
        (ch.frame < 4 ? first : second) = ch.pitch;
    }

    SEQLIB_CHECK(std::abs(first - 12 * std::log2(131072.0 / 250 / 523.2511306)) < 1e-6);
    SEQLIB_CHECK(std::abs(second - 12 * std::log2(64 / 32.70319566)) < 1e-6);
}

// An entry of 0 in the sample table points the header at the table itself. The driver plays those bytes. The SoundFont
// leaves the sample out. Here sample 1 is empty, and the table's first 16 bytes read as a header of 8 points.
void TestEmptySample()
{
    Cartridge c = TestCartridge({0xFF});
    c.U32(kSamples, 16);
    c.U32(kSamples + 4, 0);
    c.U32(kSamples + 8, 0);
    c.U32(kSamples + 12, 8);
    c.U32(kSamples + 16, 0x40000000);
    c.U32(kSamples + 20, 8000 * 1024);
    c.U32(kSamples + 24, 32);
    c.U32(kSamples + 28, 64);
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, info, error) && info.sample_count == 4);
    SampleHeader h;
    SEQLIB_CHECK(ReadSample(rom, info, 1, h) && h.empty && h.address == kSamples && h.end == 8);
    SEQLIB_CHECK(ReadSample(rom, info, 0, h) && !h.empty && h.rate == 8000 * 1024);

    Region r;
    SEQLIB_CHECK(FindRegion(rom, info, 0, 60, r));
    r.sample = 1;
    InstrumentSet empty;
    empty.Add(Kind::kSample, 0, 60, r);
    Sf2File without;

    SEQLIB_CHECK(empty.Build(rom, info, without).size() == 1); // "Instrument 0 has nothing to play"

    SEQLIB_CHECK(without.samples.empty() && without.instruments.empty());

    r.sample = 0;
    InstrumentSet full;
    full.Add(Kind::kSample, 0, 60, r);
    Sf2File with;

    SEQLIB_CHECK(full.Build(rom, info, with).empty());

    SEQLIB_CHECK(with.samples.size() == 1 && with.instruments.size() == 1);
}

// The listing reads each version's commands as its driver does.
void TestDump()
{
    const std::initializer_list<int> kTrack = {0xFA, 0x03, 0x00, 0x3C, 0x80, 0xF1, 0x3E, 0x20, 0xFF};
    DriverInfo info;
    std::string error;
    const std::string path = Utf8(TempPath("alphadream_dump.txt"));

    const auto dump = [&](const Rom& rom)
    {
        SEQLIB_CHECK(DetectDriver(rom, info, error) && DumpSong(rom, info, 1, path, error));
        const std::vector<uint8_t> text = ReadAll(path);
        return std::string(text.begin(), text.end());
    };

    // In B85A, FA sets the priority, a note's third byte is the volume, and F1 is a note's length.
    Cartridge sports = TestCartridge(kTrack);
    sports.Code(kNote, kHamSportsNoteCode);

    const std::string b85a = dump(sports.Build());

    SEQLIB_CHECK(b85a.find("  +0000  priority 3\n") != std::string::npos);
    SEQLIB_CHECK(b85a.find("  +0002  tie key 60 volume 128\n") != std::string::npos);
    SEQLIB_CHECK(b85a.find("  +0005  note key 62 length 241 volume 32\n") != std::string::npos);
    SEQLIB_CHECK(b85a.find("  +0008  end\n") != std::string::npos);

    // In A84J, FA is a note's length too.
    Cartridge older = TestCartridge(kTrack);
    older.Code(kNote, kHamtaro4NoteCode);

    const std::string a84j = dump(older.Build());

    SEQLIB_CHECK(a84j.find("  +0000  note key 3 length 250 volume 0\n") != std::string::npos);

    // In A88E, F1 is the volume and a note has two bytes.
    const std::string a88e = dump(TestCartridge({0xF1, 0x80, 0x08, 0x3C, 0xFF}).Build());

    SEQLIB_CHECK(a88e.find("  +0000  volume 128\n") != std::string::npos);
    SEQLIB_CHECK(a88e.find("  +0002  note key 60 length 8\n") != std::string::npos);
    SEQLIB_CHECK(a88e.find("  +0004  end\n") != std::string::npos);
}

// The conversion writes a track for each track that plays notes, and a preset for each instrument.
void TestConversion()
{
    const Rom rom = TestCartridge({0xF1, 0x80, 0x08, 0x3C, 0x08, 0x40, 0xF8, 0xF9, 0xFF}).Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    ConvertSettings settings = TempSettings("alphadream");

    const SongReport report = ConvertSong(rom, info, 1, settings, nullptr);

    SEQLIB_CHECK(report.ok);
    SEQLIB_CHECK(report.tracks == 2);
    SEQLIB_CHECK(report.loop_start >= 0 && report.loop_end > report.loop_start);

    const MidiEvents midi = ReadMidi(report.midi_path);
    SEQLIB_CHECK(midi.tracks.size() == 3);
    const Sf2Records sf2 = ReadSf2(report.sf2_path);
    SEQLIB_CHECK(sf2.Count("phdr", 38) == 3); // two presets and the closing record

    // At the starting tempo of 0x78 and the divisor 0x4B, 8 ticks last 5 frames. Track 2 plays keys 60 and 64 and goes
    // back to key 60 on frame 10. The note that follows the goto starts on the same frame. The loop runs from frame 10
    // to frame 20, and a note of key 60 starts on each marker.
    settings.frame_timing = true;

    const SongReport frames = ConvertSong(rom, info, 1, settings, nullptr);

    SEQLIB_CHECK(frames.ok);
    SEQLIB_CHECK(std::fabs(frames.loop_start - 10 / agblib::agb::kFrameRate) < 1e-9);
    SEQLIB_CHECK(std::fabs(frames.loop_end - 20 / agblib::agb::kFrameRate) < 1e-9);

    const MidiEvents timed = ReadMidi(frames.midi_path);
    std::vector<uint64_t> markers;
    for (const auto& e : timed.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back(e.tick);
        }
    }

    std::vector<uint64_t> starts;
    for (const auto& e : timed.tracks[1].events)
    {
        if ((e.bytes[0] & 0xF0) == 0x90 && e.bytes[1] == 60 && e.bytes[2] > 0)
        {
            starts.push_back(e.tick);
        }
    }

    SEQLIB_REQUIRE_EQ(markers.size(), 2);
    SEQLIB_CHECK(std::count(starts.begin(), starts.end(), markers[0]) == 1);
    SEQLIB_CHECK(std::count(starts.begin(), starts.end(), markers[1]) == 1);
}

// A note that its track releases on the frame it starts plays through the release, from the level that the channel had.
// At tempo 255, three or four ticks run each frame. Track 2 plays key 60 for 23 ticks, long enough to reach the sustain
// level of 0x80, then key 60 for 1 tick with a rest of 16. Frame 7 runs ticks 21 to 24, and the second note starts and
// ends on it. The release takes 0x20 a frame: the second note plays at 0x60, 0x40 and 0x20, and the channel stops on
// the fourth frame.
void TestReleasedNote()
{
    const Rom rom = TestCartridge({0xF9, 0xFF, 0x17, 0x3C, 0x01, 0xBC, 0x10, 0xFF}).Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    Song song;
    SEQLIB_CHECK(ReadSong(rom, info, 1, song));
    Sequencer seq(rom, info, song);

    std::vector<int> levels;
    for (int f = 0; f < 20; f++)
    {
        seq.Step();
        levels.push_back(seq.Channel(2).level);
    }

    std::vector<Note> notes;
    for (const Note& n : seq.Notes())
    {
        if (n.track == 2)
        {
            notes.push_back(n);
        }
    }

    SEQLIB_REQUIRE_EQ(notes.size(), 2);
    const Note& n = notes[1];
    SEQLIB_CHECK(n.released == n.on && n.ended && n.off == n.on + 4);
    SEQLIB_CHECK(levels[n.on] == 0x60 && levels[n.on + 1] == 0x40 && levels[n.on + 2] == 0x20);

    std::vector<double> shares;
    for (const Change& c : seq.Changes())
    {
        if (c.track == 2 && c.frame >= n.on)
        {
            shares.push_back(c.left / (seq.Channel(2).left * 63.0 / 127));
        }
    }

    SEQLIB_REQUIRE_EQ(shares.size(), 3);
    SEQLIB_CHECK(std::abs(shares[0] - 0x60 / 255.0) < 1e-9 && std::abs(shares[2] - 0x20 / 255.0) < 1e-9);

    // The conversion keeps the note.
    ConvertSettings settings = TempSettings("alphadream_released");

    const SongReport report = ConvertSong(rom, info, 1, settings, nullptr);

    SEQLIB_CHECK(report.ok);
    const MidiEvents midi = ReadMidi(report.midi_path);
    SEQLIB_CHECK(midi.Find(0x90 | MelodicChannel(2)).size() >= 2);
}

// A song loops for the period that the most tracks fill. At tempo 75, tracks 2 and 3 go back every 10 frames and track
// 8 every 15. The loop starts on frame 15, where track 8 first goes back, and lasts 10 frames.
void TestLoopPeriod()
{
    Cartridge c = TestCartridge({0xF9, 0x4B, 0x0A, 0x3C, 0xF8, 0xFB, 0xFF});
    c.U16(kSong + 0x10, 0x10C); // tracks 2, 3 and 8
    c.U16(kSong + 0x14, 0x40);
    c.U16(kSong + 0x16, 0x80);
    c.Bytes(kSong + 0x50, {0xF9, 0x4B, 0x0A, 0x40, 0xF8, 0xFB, 0xFF});
    c.Bytes(kSong + 0x90, {0xF9, 0x4B, 0xF0, 0x01, 0x0F, 0x48, 0xF8, 0xFB, 0xFF});
    const Rom rom = c.Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    ConvertSettings settings = TempSettings("alphadream_period");

    const SongReport report = ConvertSong(rom, info, 1, settings, nullptr);

    SEQLIB_CHECK(report.ok && report.tracks == 3);
    SEQLIB_CHECK(std::fabs(report.loop_start - 15 / agblib::agb::kFrameRate) < 1e-9);
    SEQLIB_CHECK(std::fabs(report.loop_end - 25 / agblib::agb::kFrameRate) < 1e-9);
    SEQLIB_CHECK(std::fabs(report.seconds - 35 / agblib::agb::kFrameRate) < 1e-9);
}

// The performance of TestConversion's song. Track 2 sets its volume to 0x80, plays keys 60 and 64 for 5 frames each,
// and goes back to key 60 on frames 10, 20 and 30. The song ends on frame 30, after two passes of the loop. Track 8
// sets its volume to 0xFF and the pan to the centre, and plays key 72.
void TestPerformance()
{
    const Rom rom = TestCartridge({0xF1, 0x80, 0x08, 0x3C, 0x08, 0x40, 0xF8, 0xF9, 0xFF}).Build();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));

    const Performance perf = PerformSong(rom, info, 1, TempSettings("alphadream_performance"));

    SEQLIB_CHECK(perf.base == kSong + 0x10 && perf.end_tick == 30);
    SEQLIB_CHECK(perf.loop && perf.loop->start == 10 && perf.loop->end == 20);
    SEQLIB_CHECK(perf.track_names[2] == "Track 2" && perf.track_names[8] == "Square 1");

    // Each note leads to its bytes, in the stretch that runs them: the track's start, then each pass of the loop. The
    // pass that would start on frame 30 belongs to the next one.
    std::vector<int> keys;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> stretches;
    for (const ntdlib::sequence_export::TrackEvent& e : perf.tracks[2])
    {
        if (e.kind == ntdlib::sequence_export::EventKind::kNoteOn)
        {
            keys.push_back(e.key);
            offsets.push_back(perf.note_commands.at(e.note - 1).offset);
            stretches.push_back(perf.note_commands.at(e.note - 1).stretch);
        }
    }

    SEQLIB_CHECK(keys == std::vector<int>({60, 64, 60, 64, 60, 64}));
    SEQLIB_CHECK(offsets == std::vector<uint32_t>({0x12, 0x14, 0x12, 0x14, 0x12, 0x14}));
    SEQLIB_CHECK(stretches == std::vector<uint32_t>({0, 0, 1, 1, 2, 2}));
    SEQLIB_REQUIRE_EQ(perf.stretches[2].size(), 3);
    const ntdlib::sequence_export::Stretch& open = perf.stretches[2][0];
    const ntdlib::sequence_export::Stretch& loop = perf.stretches[2][1];
    SEQLIB_CHECK(open.entry == ntdlib::sequence_export::Entry::kOpen && open.tick == 0 && open.first == 0x10 &&
                 open.last == 0x16);
    SEQLIB_CHECK(loop.entry == ntdlib::sequence_export::Entry::kLoop && loop.tick == 10 && loop.first == 0x12 &&
                 loop.last == 0x16);
    SEQLIB_CHECK(perf.midi_places.size() == perf.note_commands.size());

    // The offsets name the decoder's items.
    const agblib::alphadream::Decoded decoded =
        agblib::alphadream::DecodeRomSequence(rom, info, agblib::alphadream::SequenceOfSong(rom, info, 1));
    const std::vector<uint32_t> items = agblib::alphadream::ItemOffsets(decoded.sequence, info.version);

    const auto item = std::find(items.begin(), items.end(), 0x14u);
    SEQLIB_REQUIRE(item != items.end());
    const ntdlib::commands::Command& note = decoded.sequence.items[size_t(item - items.begin())];
    SEQLIB_CHECK(note.kind == ntdlib::commands::Kind::kNote && note.key == 64);

    // F1 sets the volume, and F2's 0x7F is the centre.
    SEQLIB_CHECK(perf.mix[2] ==
                 std::vector<ntdlib::sequence_export::MixChange>({{0, ntdlib::sequence_export::Mix::kVolume, 0x80}}));
    SEQLIB_CHECK(perf.mix[8] ==
                 std::vector<ntdlib::sequence_export::MixChange>(
                     {{0, ntdlib::sequence_export::Mix::kVolume, 0xFF}, {0, ntdlib::sequence_export::Mix::kPan, 0}}));
}

} // namespace

int RunTests()
{
    TestDetection();
    TestVersions();
    TestHamtaroNotes();
    TestSequencer();
    TestRests();
    TestPsgEnvelope();
    TestPsgRecords();
    TestWave();
    TestPsgPitch();
    TestEmptySample();
    TestDump();
    TestConversion();
    TestLoopPeriod();
    TestReleasedNote();
    TestPerformance();

    return seqlib::testing::Summary("alphadream");
}

} // namespace agblib::sequence_export::alphadream

int main()
{
    return agblib::sequence_export::alphadream::RunTests();
}
