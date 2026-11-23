// SPDX-License-Identifier: MIT

// Tests of ubimilan::Output on small synthetic cartridges: the stores of the driver's strict mode and the bytes of its
// mixer. Each expected value follows a rule of the driver's code, worked out by hand. The comments give the rule.
//
// A run of the mixer takes 8 points of each voice. Points p at volume 64 give a sum of 64p. With a point halfway
// between each pair of sums, and each point averaged with the one before, a point's two bytes come to (3 * last + p) /
// 4 and (last + 3 * p) / 4. The tests' points are multiples of 4, and the sums then round nowhere.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/ubimilan_image.h"
#include "agblib/ubimilan/driver.h"
#include "agblib/ubimilan/output.h"
#include "agblib/ubimilan/sequencer.h"
#include "ubimilan_output_test_util.h"

namespace
{

namespace ubimilan = agblib::ubimilan;
using agblib::agb::Rom;
using agblib::testing::ubimilan_image::TestImage;
using ubimilan::DriverInfo;
using ubimilan::Output;

using namespace agblib::testing::ubimilan_output_tests;

void TestRuns()
{
    // Timer 0's reload gives 1,025 cycles a point, and 8 frames hold 137.02 runs: 17 runs in each of the first 7
    // frames, and 18 in the eighth.
    TestImage image;
    const int sequence = image.AddSequence({0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    XSFLIB_CHECK(ubimilan::OutputCycles(info) == 1025);
    XSFLIB_CHECK(info.warnings.empty());
    XSFLIB_CHECK(info.fine_table == agblib::testing::ubimilan_image::kFineTable);
    Output output(rom, info, 0);
    for (int frame = 0; frame < 7; frame++)
    {
        output.Frame();
        XSFLIB_CHECK(output.Fifo().size() == 17 * kRunBytes);
    }

    output.Frame();
    XSFLIB_CHECK(output.Fifo().size() == 18 * kRunBytes);
}

void TestVoiceBytes()
{
    // Key 40 on the first frame: the constant points of 64 at volume 64 come in over a point's two bytes.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(4000, 64)));
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);
    SEQLIB_REQUIRE(played.bytes.size() == 17 * kRunBytes);
    XSFLIB_CHECK(played.bytes[0] == 16); // (3 * 0 + 64) / 4
    XSFLIB_CHECK(played.bytes[1] == 48); // (0 + 3 * 64) / 4
    for (size_t i = 2; i < played.bytes.size(); i++)
    {
        XSFLIB_CHECK(played.bytes[i] == 64);
    }
}

void TestVolume()
{
    // A byte is the sum over 64: a point of 64 at volume 16 gives 16. A negative sum rounds down: -33 at volume 1 gives
    // -33 / 64. An arithmetic shift of each half takes that to -1.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(4000, 64)));
    image.SetKey(41, image.AddSample(Constant(4000, -33)));
    const int sequence = image.AddSequence(
        {0x00, 0x00, 0x99, 40, 16, 0x08, 0x00, 0x89, 40, 0, 0x00, 0x00, 0x99, 41, 1, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 20);
    XSFLIB_CHECK(played.bytes[100] == 16);
    XSFLIB_CHECK(played.bytes.back() == -1);

    // Each half rounds down: a sum of -65 halves to -33, and -33 and -33 over 64 give -2. A division towards 0 would
    // give -32 and -32, and -1.
    TestImage halves;
    halves.SetKey(40, halves.AddSample(Constant(4000, -65)));
    halves.AddPiece(0, {halves.AddSequence({0x00, 0x00, 0x99, 40, 1, 0x20, 0x00, 0xFC})});
    XSFLIB_CHECK(Play(halves, 2).bytes.back() == -2);

    // A point in between rounds down too. Between sums of -126 and -43, it's -85, whose half is -43. Every byte then
    // gives -2: (-63 - 43) / 64 and (-43 - 22) / 64. A division towards 0 would give -84 and -42, and bytes of -1.
    std::vector<int8_t> points;
    for (int i = 0; i < 2000; i++)
    {
        points.push_back(-126);
        points.push_back(-43);
    }

    TestImage between;
    between.SetKey(40, between.AddSample(points));
    between.AddPiece(0, {between.AddSequence({0x00, 0x00, 0x99, 40, 1, 0x20, 0x00, 0xFC})});
    const std::vector<int8_t> between_bytes = Play(between, 2).bytes;
    for (size_t i = between_bytes.size() - kRunBytes; i < between_bytes.size(); i++)
    {
        XSFLIB_CHECK(between_bytes[i] == -2);
    }
}

void TestLastPointsLeftOut()
{
    // A run moves a voice on 8 points, and adds its points only if the move leaves it playing. A sample of 16 points
    // stops in its second run, whose points stay out: only 8 are heard. After them, the sums fall in the same two steps
    // as they rose.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(16, 64)));
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);
    XSFLIB_CHECK(played.bytes[15] == 64);
    XSFLIB_CHECK(played.bytes[16] == 48);
    XSFLIB_CHECK(played.bytes[17] == 16);
    XSFLIB_CHECK(played.bytes[18] == 0);

    // One more point takes the voice through a second run: 16 points are heard.
    TestImage longer;
    longer.SetKey(40, longer.AddSample(Constant(17, 64)));
    const int same = longer.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC});
    longer.AddPiece(0, {same});
    const Played played_longer = Play(longer, 1);
    XSFLIB_CHECK(played_longer.bytes[31] == 64);
    XSFLIB_CHECK(played_longer.bytes[32] == 48);
    XSFLIB_CHECK(played_longer.bytes[33] == 16);
    XSFLIB_CHECK(played_longer.bytes[34] == 0);
}

void TestLoop()
{
    // A voice goes back to its loop's start once a run takes it to the loop's end. A DMA word transfer reads from the
    // word that holds the position: a loop that starts at point 10 plays from point 8.
    std::vector<int8_t> points;
    for (int i = 0; i < 16; i++)
    {
        points.push_back(int8_t(4 * i));
    }

    TestImage image;
    image.SetKey(40, image.AddSample(points, true, 10, 16));
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);

    // The first run plays points 0 to 7, the second points 8 to 15, and each run after them points 8 to 15 again.
    const auto point_bytes = [&](size_t run, int point, int last)
    {
        const size_t at = run * kRunBytes + 2 * size_t(point);
        XSFLIB_CHECK(played.bytes[at] == int8_t((3 * 4 * last + 4 * (point + (run ? 8 : 0))) / 4));
        XSFLIB_CHECK(played.bytes[at + 1] == int8_t((4 * last + 3 * 4 * (point + (run ? 8 : 0))) / 4));
    };

    for (int i = 1; i < 8; i++)
    {
        point_bytes(0, i, i - 1);
    }

    point_bytes(1, 0, 7);
    for (int i = 1; i < 8; i++)
    {
        point_bytes(1, i, 8 + i - 1);
        point_bytes(2, i, 8 + i - 1);
    }

    point_bytes(2, 0, 15);
}

void TestClip()
{
    // Each byte clips to 8 bits after the voices' sum: 4 voices of 100 at volume 64 give 400.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(4000, 100)));
    const std::vector<uint8_t> four_high = {0x00, 0x00, 0x99, 40,   64,   0x00, 0x00, 0x99, 40,   64,   0x00, 0x00,
                                            0x99, 40,   64,   0x00, 0x00, 0x99, 40,   64,   0x20, 0x00, 0xFC};
    image.AddPiece(0, {image.AddSequence(four_high)});
    XSFLIB_CHECK(Play(image, 1).bytes[100] == 127);

    TestImage other;
    other.SetKey(41, other.AddSample(Constant(4000, -100)));
    const std::vector<uint8_t> four_low = {0x00, 0x00, 0x99, 41,   64,   0x00, 0x00, 0x99, 41,   64,   0x00, 0x00,
                                           0x99, 41,   64,   0x00, 0x00, 0x99, 41,   64,   0x20, 0x00, 0xFC};
    other.AddPiece(0, {other.AddSequence(four_low)});
    XSFLIB_CHECK(Play(other, 1).bytes[100] == -128);
}

void TestVoices()
{
    // The kit's notes take the last voice that no sound has. A note-off stops the voice of the kit's last note, and the
    // next note takes it. Levels of 8, 16 and 32 tell the voices apart.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(40000, 8)));
    image.SetKey(41, image.AddSample(Constant(40000, 16)));
    image.SetKey(42, image.AddSample(Constant(40000, 32)));
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x99, 40,   64,   0x00, 0x00, 0x99, 41, 64, 0x02, 0x00, // 2 voices, 3 frames
        0x89, 41,   0,    0x02, 0x00,                                       // stops key 41's voice, 3 frames
        0x99, 42,   64,   0x20, 0x00,                                       // takes it
        0xFC,
    });

    image.AddPiece(0, {sequence});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 24);
    output.Frame();
    output.Frame();
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 8);
    output.Frame();
    output.Frame();
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 40);
}

void TestVoiceChoices()
{
    // A note-off frees its voice at once: a note in the same frame takes the voice again.
    TestImage same;
    same.SetKey(40, same.AddSample(Constant(40000, 8)));
    same.AddPiece(0, {same.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0x89, 40, 0, 0x00, 0x00, 0x99, 40, 64,
                                        0x20, 0x00, 0xFC})});
    const Rom same_rom = same.ToRom();
    const DriverInfo same_info = Detect(same_rom);
    Output same_output(same_rom, same_info, 0);
    same_output.Frame();
    XSFLIB_CHECK(same_output.Song().VoicePlays(3));
    XSFLIB_CHECK(!same_output.Song().VoicePlays(2));

    // A sample that has ended leaves its voice free for the next frame's sound update. 64 points end in the first
    // frame.
    TestImage ended;
    ended.SetKey(40, ended.AddSample(Constant(64, 8)));
    ended.SetKey(41, ended.AddSample(Constant(40000, 16)));
    ended.AddPiece(0, {ended.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x01, 0x00, 0x99, 41, 64, 0x20, 0x00, 0xFC})});
    const Rom ended_rom = ended.ToRom();
    const DriverInfo ended_info = Detect(ended_rom);
    Output ended_output(ended_rom, ended_info, 0);
    ended_output.Frame();
    XSFLIB_CHECK(!ended_output.Song().VoicePlays(3));
    ended_output.Frame();
    ended_output.Frame();
    XSFLIB_CHECK(ended_output.Song().VoicePlays(3));
    XSFLIB_CHECK(!ended_output.Song().VoicePlays(2));

    // Channel 9's programs from 12 up move its keys on through the kit. Program 11 goes to the PSG.
    const auto level_after = [](int program)
    {
        TestImage image;
        image.SetKey(40, image.AddSample(Constant(40000, 8)));
        image.SetKey(40 + program, image.AddSample(Constant(40000, 16)));
        image.AddPiece(
            0, {image.AddSequence({0x00, 0x00, 0xC9, uint8_t(program), 0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC})});
        return int(Play(image, 1).bytes.back());
    };

    XSFLIB_CHECK(level_after(12) == 16);
    XSFLIB_CHECK(level_after(11) == 8);

    // A sample without a loop keeps the last loop's points in the voice's record.
    TestImage kept;
    kept.SetKey(40, kept.AddSample(Constant(4000, 8), true, 16, 3200));
    kept.SetKey(41, kept.AddSample(Constant(40000, 16)));
    kept.AddPiece(8, {kept.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x01, 0x00, 0x99, 41, 64, 0x20, 0x00, 0xFC})});
    const Rom kept_rom = kept.ToRom();
    const DriverInfo kept_info = Detect(kept_rom);
    Output kept_output(kept_rom, kept_info, 0);
    kept_output.Frame();
    const std::array<uint32_t, 6> looping = kept_output.Song().VoiceState(3);
    kept_output.Frame();
    kept_output.Frame();
    const std::array<uint32_t, 6> once = kept_output.Song().VoiceState(3);
    XSFLIB_CHECK(looping[5] == 1u);
    XSFLIB_CHECK(once[5] == 0u);
    XSFLIB_CHECK(once[1] == looping[1]);
    XSFLIB_CHECK(once[3] == looping[3]);

    // Only a byte of 1 makes a sample loop.
    TestImage byte;
    const int sample = byte.AddSample(Constant(64, 8), true, 0, 64);
    byte.Put8(byte.Resource(sample) + 1, 2);
    byte.SetKey(40, sample);
    byte.AddPiece(0, {byte.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC})});
    const Rom byte_rom = byte.ToRom();
    const DriverInfo byte_info = Detect(byte_rom);
    Output byte_output(byte_rom, byte_info, 0);
    byte_output.Frame();
    XSFLIB_CHECK(!byte_output.Song().VoicePlays(3));
}

void TestVoiceMinusOne()
{
    // A fifth note finds no voice. The game takes voice -1, whose second record is voice 3's first: the note replaces
    // the sample of the first note's voice 3. A note-off then has no voice to stop, whatever its key.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(40000, 4)));
    image.SetKey(41, image.AddSample(Constant(40000, 8)));
    image.SetKey(42, image.AddSample(Constant(40000, 16)));
    image.SetKey(43, image.AddSample(Constant(40000, 32)));
    image.SetKey(44, image.AddSample(Constant(40000, 40)));
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x99, 40,   64,   0x00, 0x00, 0x99, 41,   64,   0x00, 0x00, 0x99, 42,   64,   0x00, 0x00,
        0x99, 43,   64,   0x00, 0x00, 0x99, 44,   64,   0x02, 0x00, 0x89, 44,   0,    0x20, 0x00, 0xFC,
    });

    image.AddPiece(0, {sequence});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 8 + 16 + 32 + 40);
    XSFLIB_CHECK(output.Song().VoicePlays(3));
    for (int frame = 0; frame < 6; frame++)
    {
        output.Frame();
    }

    XSFLIB_CHECK(output.Fifo().back() == 8 + 16 + 32 + 40);
}

void TestKeptVoice()
{
    // A piece with bit 3 keeps voice 3 for the kit: each note replaces the last, and the voice is never free.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(40000, 8)));
    image.SetKey(41, image.AddSample(Constant(40000, 16)));
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0x99, 41, 64, 0x20, 0x00, 0xFC});
    image.AddPiece(8, {sequence});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    output.Frame();
    XSFLIB_CHECK(output.Fifo().back() == 16);
    XSFLIB_CHECK(output.Song().VoicePlays(3));
    XSFLIB_CHECK(!output.Song().VoicePlays(2));
}

void TestPieces()
{
    // A looping piece of two sequences plays them in turn. Each sequence's first countdown follows the last command's
    // frame. Levels of 8 and 16 tell the sequences apart.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(40000, 8)));
    image.SetKey(41, image.AddSample(Constant(40000, 16)));
    const int first = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x02, 0x00, 0x89, 40, 0, 0x02, 0x00, 0xFC});
    const int second = image.AddSequence({0x00, 0x00, 0x99, 41, 64, 0x02, 0x00, 0x89, 41, 0, 0x02, 0x00, 0xFC});
    image.AddPiece(1, {first, second});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    std::vector<int> levels;
    for (int frame = 0; frame < 25; frame++)
    {
        output.Frame();
        levels.push_back(output.Fifo().back());
    }

    XSFLIB_CHECK(!output.Song().Reset());
    XSFLIB_CHECK(levels ==
                 std::vector<int>({8, 8, 8, 0, 0, 0, 16, 16, 16, 0, 0, 0, 8, 8, 8, 0, 0, 0, 16, 16, 16, 0, 0, 0, 8}));
}

void TestResets()
{
    // The game resets on a status byte that it doesn't take.
    TestImage unknown;
    unknown.AddPiece(0, {unknown.AddSequence({0x00, 0x00, 0xA0, 0, 0, 0xFC})});
    XSFLIB_CHECK(Play(unknown, 1).reset);

    // On a voice's volume above 64, in the sound update of the frame that starts the note.
    TestImage loud;
    loud.SetKey(40, loud.AddSample(Constant(4000, 64)));
    loud.AddPiece(0, {loud.AddSequence({0x00, 0x00, 0x99, 40, 65, 0x20, 0x00, 0xFC})});
    const Played played_loud = Play(loud, 1);
    XSFLIB_CHECK(played_loud.reset);
    for (const int8_t byte : played_loud.bytes)
    {
        XSFLIB_CHECK(byte == 0);
    }

    // A volume of 64 doesn't.
    TestImage fine;
    fine.SetKey(40, fine.AddSample(Constant(4000, 64)));
    fine.AddPiece(0, {fine.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC})});
    XSFLIB_CHECK(!Play(fine, 1).reset);

    // A sample among the piece's sequences leaves the resource that the game's callback gave the track. The first end
    // therefore passes. The second end takes the sample's bytes for a sequence: its kind for a countdown of 3, then a
    // status of 0x40.
    TestImage listed;
    const int listed_sample = listed.AddSample(Constant(64, 8));
    const int listed_first = listed.AddSequence({0x00, 0x00, 0xE0, 0x01, 0x00, 0xFC});
    const int listed_second = listed.AddSequence({0x00, 0x00, 0xE0, 0x10, 0x00, 0xFC});
    listed.AddPiece(1, {listed_first, listed_second, listed_sample});
    XSFLIB_CHECK(!Play(listed, 23).reset);
    XSFLIB_CHECK(Play(listed, 24).reset);

    // And at the first end of a piece of two sequences that doesn't loop: the game reads the kind of the sequence after
    // the next, and finds none.
    TestImage two;
    const int first = two.AddSequence({0x00, 0x00, 0xE0, 0x01, 0x00, 0xFC});
    const int second = two.AddSequence({0x08, 0x00, 0xFC});
    two.AddPiece(0, {first, second});
    XSFLIB_CHECK(!Play(two, 2).reset);
    XSFLIB_CHECK(Play(two, 3).reset);

    // A piece of one sequence just ends.
    TestImage one;
    one.AddPiece(0, {one.AddSequence({0x00, 0x00, 0xFC})});
    const Rom rom = one.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    output.Frame();
    XSFLIB_CHECK(!output.Song().Reset());
    XSFLIB_CHECK(!output.Song().TrackActive());
}

void TestInvalid()
{
    // A piece whose first resource isn't a sequence can't play.
    TestImage image;
    const int sample = image.AddSample(Constant(16, 0));
    image.AddPiece(0, {sample});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    XSFLIB_CHECK(!Output(rom, info, 0).Valid());
    XSFLIB_CHECK(!Output(rom, info, 5).Valid());
}

void TestVoicesSilent()
{
    // A voice that a run stops counts as silent. So do voices that a note-off stops.
    TestImage image;
    image.SetKey(40, image.AddSample(Constant(64, 64)));
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x20, 0x00, 0xFC})});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    XSFLIB_CHECK(output.VoicesSilent());
    output.Frame();
    XSFLIB_CHECK(output.VoicesSilent());

    TestImage looped;
    looped.SetKey(40, looped.AddSample(Constant(64, 64), true, 0, 64));
    looped.AddPiece(0, {looped.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x01, 0x00, 0x89, 40, 0, 0x20, 0x00, 0xFC})});
    const Rom rom_looped = looped.ToRom();
    const DriverInfo info_looped = Detect(rom_looped);
    Output output_looped(rom_looped, info_looped, 0);
    output_looped.Frame();
    XSFLIB_CHECK(!output_looped.VoicesSilent());
    output_looped.Frame();
    output_looped.Frame();
    XSFLIB_CHECK(output_looped.VoicesSilent());

    // The sequencer plays on until its track has ended and its voices have stopped. 1,600 points last 200 of the
    // mixer's runs, and 12 frames hold 205.
    TestImage tail;
    tail.SetKey(40, tail.AddSample(Constant(1600, 64)));
    tail.AddPiece(0, {tail.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0xFC})});
    const Rom rom_tail = tail.ToRom();
    const DriverInfo info_tail = Detect(rom_tail);
    Output output_tail(rom_tail, info_tail, 0);
    for (int frame = 0; frame < 11; frame++)
    {
        output_tail.Frame();
    }

    XSFLIB_CHECK(!output_tail.Song().TrackActive());
    XSFLIB_CHECK(output_tail.Song().Playing());
    output_tail.Frame();
    XSFLIB_CHECK(!output_tail.Song().Playing());
}

} // namespace

int main()
{
    TestRuns();
    TestVoiceBytes();
    TestVolume();
    TestLastPointsLeftOut();
    TestLoop();
    TestClip();
    TestVoices();
    TestVoiceChoices();
    TestVoiceMinusOne();
    TestKeptVoice();
    TestPieces();
    TestResets();
    TestInvalid();
    TestVoicesSilent();
    return xsflib::testing::Summary("Ubisoft Milan's output");
}
