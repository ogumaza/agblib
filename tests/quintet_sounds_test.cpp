// SPDX-License-Identifier: MIT

// Unit tests of the sounds of Quintet's driver: the registers, the wave volumes, PCM, auditions,
// instruments, banks and the noise channel's drums. They build small synthetic cartridges in memory. No
// game ROM is needed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "agblib/agb/files.h"
#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/quintet/archive_player.h"
#include "agblib/quintet/commands.h"
#include "agblib/quintet/driver.h"
#include "agblib/quintet/instruments.h"
#include "agblib/quintet/sequencer.h"
#include "agblib/quintet/song.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/psg.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/quintet/convert.h"
#include "agblib/sequence_export/quintet/soundfont.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/quintet_cart.h"
#include "quintet_test_util.h"

namespace agblib::sequence_export::quintet
{
namespace
{

using seqlib::testing::Throws;

void TestRegisters()
{
    // A note on square 1: the frequency table's entry for octave 2's C, and the note's volume, duty and length.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x5E, 12, 0x2F, 1}).Note(0, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto writes = Writes(rom, info, 0, 2);

    SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x04000062), 0xC87F);
    SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x04000064), 0x8000 | rom.U16(kFrequencyTable + 48));
}

void TestWaveVolumes()
{
    // The wave channel at volume 3 plays at 100% in the A revision, and at 75% in the J revision. The J revision has 4
    // for 100%. NR32's volume code is the top bits of NR31's halfword. The driver fills that halfword with 4 times the
    // length.
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        Cart cart(revision);
        cart.Songs(
            {Song({{0xFF}, {0xFF}, Channel().Bytes({0xDA, 0x00, 0x5E, 3}).Note(0, 3).Bytes({0x5F}).Note(0, 3).End()})});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        const auto writes = Writes(rom, info, 0, 60);
        const long long first = LastWrite(writes[1], 0x04000072);
        long long second = -1;
        for (size_t f = 2; f < writes.size() && second < 0; f++)
        {
            second = LastWrite(writes[f], 0x04000072);
        }

        if (revision == Revision::kA)
        {
            SEQLIB_CHECK_EQ(first, 0x20FC);
            SEQLIB_CHECK_EQ(second, 0x20FC);
        }
        else
        {
            SEQLIB_CHECK_EQ(first, 0x80FC);
            SEQLIB_CHECK_EQ(second, 0x20FC);
        }
    }
}

void TestPcm()
{
    // A PCM note points FIFO A's DMA 32 bytes into the sample, the 32 before going into the FIFO, and sets timer 0 for
    // the sample's rate. The J revision sets the timer at the end of the frame.
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        Cart cart(revision);
        cart.Songs({Song({{0xFF}, {0xFF}, {0xFF}, {0xFF}, Channel().Bytes({0xDC, 0x00, 0x5E, 1}).Note(12, 3).End()})});
        const std::vector<uint32_t> data = cart.Samples(kSampleFile, {{Saw(4000), 10000}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        const auto writes = Writes(rom, info, 0, 2);
        const std::vector<RegisterWrite>& frame = writes[1];

        SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000BC), data[0] + 32);
        SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000C6), 0xB640);
        SEQLIB_CHECK_EQ(LastWrite(frame, 0x04000100), 0x800000 | (0x10000 - 16780000 / 10000));
        SEQLIB_CHECK(!frame.empty() && (frame.back().address == 0x04000100) == (revision == Revision::kJ));
    }

    // The driver checks a FIFO once a frame, and stops or loops it when the next frame would reach the end: from 0, at
    // 600 60ths of a byte a frame and an end of 100 bytes, it does so at the 9th check, after 8 frames.
    SEQLIB_CHECK_EQ(FifoFrames(0, 600, 100), 8);
    SEQLIB_CHECK_EQ(FifoFrames(5000, 600, 100), 0);
    SEQLIB_CHECK_EQ(FifoFrames(0, 0, 100), 0);
}

// The driver starts a FIFO's timer with one 32-bit store to TMxCNT_L. Its third byte sets the start bit: bit 7 of
// TMxCNT_H. A store of 0 stops the timer, and a 16-bit store to TMxCNT_H starts timer 1 for FIFO B. On the player, a
// PCM note on FIFO A sounds with no other channel playing, and the player stops at the sample's end.
void TestPcmPlays()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({{0xFF}, {0xFF}, {0xFF}, {0xFF}, Channel().Bytes({0xDC, 0x00, 0x5E, 1}).Note(12, 3).End()})});
    const std::vector<uint32_t> data = cart.Samples(kSampleFile, {{Saw(4000), 10000}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    agblib::quintet::SoundBus bus(rom, agblib::quintet::InitRegisters(Revision::kA), true);
    bus.Store({0x040000BC, 4, data[0] + 32});
    bus.Store({0x040000C4, 4, 0xB6400000});
    bus.Store({0x04000100, 4, 0x0080FA0F});
    const bool a_runs = bus.FifoRunning(0);
    bus.Store({0x04000100, 4, 0});
    const bool a_stops = !bus.FifoRunning(0);
    bus.Store({0x040000C8, 4, data[0] + 32});
    bus.Store({0x040000D0, 4, 0xB6400000});
    bus.Store({0x04000104, 2, 0xFA0F});
    bus.Store({0x04000106, 2, 0x0080});
    const bool b_runs = bus.FifoRunning(1);

    ArchivePlayer player(rom, std::span<const DriverInfo>(&info, 1));
    player.Start(0);
    player.MeterTracks(true);
    std::vector<int16_t> out;
    int32_t peak = 0;
    for (int f = 0; f < 8; f++)
    {
        player.RunFrame(out);
        peak = std::max(peak, player.TrackPeaks(4)[0]);
    }

    const bool sounds = std::any_of(out.begin(), out.end(), [&out](int16_t v) { return v != out.front(); });
    int frames = 8;
    for (; frames < 600 && player.Busy(); frames++)
    {
        player.RunFrame();
    }

    SEQLIB_CHECK(a_runs && a_stops && b_runs);
    SEQLIB_CHECK(peak > 0);
    SEQLIB_CHECK(sounds);
    SEQLIB_CHECK(frames < 600);
}

// An audition plays one note with the instrument, the pitch and the volume given, as the channel's commands would: on
// a FIFO, the sample at index 24 plays at its rate. Its release ends the channel. On the player, a square's audition
// sounds until its release, and the player then stops.
void TestAudition()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90}).Rest(3).End()})});
    const std::vector<uint32_t> data = cart.Samples(kSampleFile, {{Saw(4000), 10000}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    seq.Audition(4, 0, 24, 1);
    const std::vector<RegisterWrite> frame = seq.Step();
    const bool playing = !seq.Ended();
    seq.ReleaseAudition();
    seq.Step();

    SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000BC), data[0] + 32);
    SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000C6), 0xB640);
    SEQLIB_CHECK_EQ(LastWrite(frame, 0x04000100), 0x800000 | (0x10000 - 16780000 / 10000));
    SEQLIB_CHECK(playing && seq.Ended());

    ArchivePlayer player(rom, std::span<const DriverInfo>(&info, 1));
    player.StartNote(0, 0, 2, 24, 15);
    std::vector<int16_t> out;
    for (int f = 0; f < 4; f++)
    {
        player.RunFrame(out);
    }

    const bool sounds = std::any_of(out.begin(), out.end(), [&out](int16_t v) { return v != out.front(); });
    const bool busy = player.Busy();
    player.ReleaseNote();
    int frames = 0;
    for (; frames < 10 && player.Busy(); frames++)
    {
        player.RunFrame(out);
    }

    SEQLIB_CHECK(sounds && busy);
    SEQLIB_CHECK(frames > 0 && frames < 10);

    // An audition that can't start leaves the music file of the song that plays.
    DriverInfo second = info;
    second.first_song = SongCount(info);
    const std::vector<DriverInfo> files = {info, second};
    ArchivePlayer two(rom, files);
    two.Start(0);
    const bool refused = Throws<std::out_of_range>([&] { two.StartNote(SongCount(info), 9, 2, 24, 15); });

    SEQLIB_CHECK(refused && two.Info().first_song == 0);
}

// A FIFO's instrument is a sample of the bank, the wave channel's a wave of the wave table, and a square's a duty. A
// sample of 106 bytes with its header plays (106 - 64) × 0.994 = 41 of them. The second wave is a square, and bit 7 of
// its number picks the wave RAM's bank.
void TestInstruments()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90}).Rest(3).End()})});
    const std::vector<uint32_t> data = cart.Samples(kSampleFile, {{Saw(100), 10000}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const Instrument pcm = LookUpInstrument(rom, info, 5, 0);
    const Instrument missing = LookUpInstrument(rom, info, 4, 1);
    const Instrument wave = LookUpInstrument(rom, info, 2, 0x81);
    const Instrument square = LookUpInstrument(rom, info, 1, 3);
    const auto sample = ReadSample(rom, info, 0, 0);
    const auto shape = ReadWave(rom, info, 0x81);

    SEQLIB_CHECK(pcm.kind == Instrument::Kind::kPcm && pcm.address == data[0] - 6 && missing.address == 0);
    SEQLIB_CHECK(wave.kind == Instrument::Kind::kWave && wave.address == FileAddress(kWaveFile) + 18);
    SEQLIB_CHECK(square.kind == Instrument::Kind::kSquare && square.number == 3);
    SEQLIB_REQUIRE(sample && shape);
    SEQLIB_CHECK(sample->data == data[0] && sample->size == 100 && sample->played == 41 && sample->rate == 10000);
    SEQLIB_CHECK((*shape)[0] == 0xFF && (*shape)[8] == 0);
    SEQLIB_CHECK(!ReadSample(rom, info, 0, 1));
}

void TestBanks()
{
    // In the J revision, F0 switches a PCM channel to the sound effects' samples. The samples that the channel then
    // selects come from there.
    Cart cart(Revision::kJ);
    cart.Songs({Song({{0xFF},
                      {0xFF},
                      {0xFF},
                      {0xFF},
                      Channel()
                          .Bytes({0xDC, 0x00, 0x5E, 1})
                          .Note(12, 3)
                          .Bytes({0xF0, 0xDC, 0x01})
                          .Note(12, 3)
                          .Bytes({0xDC, 0x00})
                          .Note(12, 3)
                          .End()})});
    const std::vector<uint32_t> music = cart.Samples(kSampleFile, {{Saw(1500), 10000}, {Saw(1500), 10000}});
    const std::vector<uint32_t> effects = cart.Samples(kSfxSampleFile, {{Saw(1500), 10000}, {Saw(1500), 10000}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto writes = Writes(rom, info, 0, 200);
    std::vector<long long> sources;
    for (size_t f = 1; f < writes.size(); f++)
    {
        const long long source = LastWrite(writes[f], 0x040000BC);
        if (source >= 0)
        {
            sources.push_back(source - 32);
        }
    }

    SEQLIB_REQUIRE(sources.size() >= 3);
    SEQLIB_CHECK_EQ(sources[0], music[0]);
    SEQLIB_CHECK_EQ(sources[1], effects[1]);
    SEQLIB_CHECK_EQ(sources[2], effects[0]);
}

void TestNoiseDrums()
{
    // D2 fades the noise channel from volume 15 to 0 over 3 frames, restarting it at each new volume, and the drum's
    // sample has those volumes in it. Two notes with the same writes share a drum.
    Cart cart(Revision::kA);
    cart.Songs({Song(
        {{0xFF}, {0xFF}, {0xFF}, Channel().Bytes({0x5E, 15, 0xD2, 100, 0, 0, 3, 1}).Note(12, 3).Note(12, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("noise");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const Sf2Records sf2 = ReadSf2(sum.sf2_path);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x93);
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(ons.size() == 2 && ons[0].bytes[1] == ons[1].bytes[1]);
    SEQLIB_CHECK_EQ(sf2.Count("shdr", 46), 2);

    // The sample's peak in each of its first 4 frames of 1097 points.
    const std::vector<uint8_t>& pcm = sf2.chunks.at("smpl");
    const uint32_t start = Le32(sf2.Record("shdr", 46, 0) + 20);
    std::vector<int> volumes;
    for (uint32_t f = 0; f < 4; f++)
    {
        int peak = 0;
        for (uint32_t i = 100; i < 1000; i++)
        {
            const uint32_t at = 2 * (start + f * 1097 + i);
            peak = std::max(peak, std::abs(int(int16_t(Le16(&pcm[at])))));
        }
        volumes.push_back(int(std::lround(peak * 15.0 / 32767)));
    }

    SEQLIB_CHECK(volumes == std::vector<int>({15, 10, 5, 0}));
}

void TestNoiseMacros()
{
    // A noise macro sets NR43 a frame at a time, restarting the channel with the note, and its end silences the
    // channel. In the J revision, F0 switches the noise channel to the sound effects' macros.
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        const bool j = revision == Revision::kJ;
        Cart cart(revision);
        std::vector<uint8_t> commands = {0x5E, 15, 0xDB, 0x01};
        if (j)
        {
            commands.insert(commands.begin(), 0xF0);
        }

        cart.Songs({Song({{0xFF}, {0xFF}, {0xFF}, Channel().Bytes(commands).Note(12, 3).End()})});
        cart.Macros(kMacroFile, {{0x27, 0x35, 0xFF}});
        cart.Macros(kSfxMacroFile, {{0x45, 0x56, 0xFF}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        const auto writes = Writes(rom, info, 0, 4);

        SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x0400007C), j ? 0x8045 : 0x8027);
        SEQLIB_CHECK_EQ(LastWrite(writes[2], 0x0400007C), j ? 0x0056 : 0x0035);
        SEQLIB_CHECK_EQ(LastWrite(writes[3], 0x0400007C), 0x8000);
        SEQLIB_CHECK_EQ(LastWrite(writes[3], 0x04000078) >> 12, 0);
    }
}

} // namespace

int RunTests()
{
    TestRegisters();
    TestWaveVolumes();
    TestPcm();
    TestPcmPlays();
    TestAudition();
    TestInstruments();
    TestBanks();
    TestNoiseDrums();
    TestNoiseMacros();

    return seqlib::testing::Summary("quintet_sounds");
}

} // namespace agblib::sequence_export::quintet

int main()
{
    return agblib::sequence_export::quintet::RunTests();
}
