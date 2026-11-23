// SPDX-License-Identifier: MIT

// Unit tests for Webfoot Technologies' driver. They build small synthetic cartridges in memory. A test reads no game's
// ROM. The expected values come from the driver's rules that the comments of `agblib/webfoot/player.h` give, and the
// crafted module's from the games' copies of the driver.

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/webfoot_cart.h"
#include "agblib/webfoot/driver.h"
#include "agblib/webfoot/player.h"
#include "module_test_util.h"

namespace agblib::webfoot
{
namespace
{

using agblib::testing::module_tests::Fnv1a;
using namespace agblib::testing::webfoot_cart;

void TestDetection()
{
    TestCart cart(Build::k2003);
    cart.AddSample({});
    cart.AddSong({Pattern({NoteEntry(0, 60, 0, 64)}, 4)}, 125, 6);
    cart.AddSong({Pattern({NoteEntry(1, 48, 0, 32)}, 8), Pattern({}, 2)}, 150, 3);
    const Rom rom = cart.ToRom();

    DriverInfo info;
    std::string error;
    SEQLIB_REQUIRE(DetectDriver(rom, info, error));
    SEQLIB_CHECK(error.empty());
    SEQLIB_REQUIRE_EQ(info.song_tables.size(), size_t(1));
    const SongTable& table = info.song_tables[0];
    SEQLIB_CHECK(table.build == Build::k2003);
    SEQLIB_CHECK_EQ(table.tables.steps, kSteps);
    SEQLIB_CHECK_EQ(table.tables.slide_up, kFineUp + 128);
    SEQLIB_CHECK_EQ(table.tables.slide_down, kFineDown + 128);
    SEQLIB_CHECK_EQ(table.tables.vibrato, kVibratoTable);
    SEQLIB_CHECK_EQ(table.address, kSongs);
    SEQLIB_CHECK_EQ(table.program, kRomBase);
    SEQLIB_CHECK_EQ(table.music_volume, kStartMusicVolume);
    SEQLIB_REQUIRE_EQ(table.songs.size(), size_t(2));
    SEQLIB_CHECK_EQ(info.SongCount(), 2);
    SEQLIB_CHECK_EQ(table.songs[1].tempo, 150);
    SEQLIB_CHECK_EQ(table.songs[1].speed, 3);
    SEQLIB_CHECK_EQ(table.songs[1].samples, kSamples);
    SEQLIB_CHECK_EQ(OrderCount(rom, table.songs[1]), 2);
    SEQLIB_REQUIRE_EQ(info.warnings.size(), size_t(1));

    // A ROM without the table of steps shows no sign of the driver.
    CartImage empty(0x4000);
    DriverInfo none;
    SEQLIB_CHECK(!DetectDriver(empty.ToRom(), none, error));
    SEQLIB_CHECK(error.empty());
}

void TestDetection2002()
{
    TestCart cart(Build::k2002);
    cart.AddSample({});
    cart.AddSong({Pattern({NoteEntry(0, 60, 0, 64)}, 4)}, 125, 6);
    cart.AddSong({Pattern({NoteEntry(0, 72, 0, 64)}, 4)}, 140, 6);
    const Rom rom = cart.ToRom();

    DriverInfo info;
    std::string error;
    SEQLIB_REQUIRE(DetectDriver(rom, info, error));
    SEQLIB_REQUIRE_EQ(info.song_tables.size(), size_t(1));
    const SongTable& table = info.song_tables[0];
    SEQLIB_CHECK(table.build == Build::k2002);
    SEQLIB_REQUIRE_EQ(table.songs.size(), size_t(2));
    SEQLIB_CHECK_EQ(table.songs[0].samples, kSamples);
    SEQLIB_CHECK_EQ(table.songs[0].masks, kMasks);
    SEQLIB_CHECK_EQ(table.songs[1].tempo, 140);

    // The 2002 mixer: 14,000 Hz for its steps, and a tick of 35,000 / tempo samples.
    const Mixer m = MixerOf(Build::k2002);
    SEQLIB_CHECK_EQ(m.step_factor, (0x3FFFFFFu + 14000) / 14000);
    SEQLIB_CHECK_EQ(m.timer_cycles, (m.step_factor + 3 + 3) / 4);
    SEQLIB_CHECK(m.byte_levels);
}

// Returns the count of `info`'s warnings that contain `text`.
int WarningsWith(const DriverInfo& info, const std::string& text)
{
    int count = 0;
    for (const std::string& w : info.warnings)
    {
        count += w.find(text) != std::string::npos ? 1 : 0;
    }

    return count;
}

// Detection reads the mixer's settings from the init: the 2002 build's rate from the game's call, the later build's
// reload of timer 0 from its literal. Settings other than MixerOf()'s, and a rate that detection can't read, give a
// warning and the song table's result.
void TestMixerCheck()
{
    for (const Build build : {Build::k2002, Build::k2003})
    {
        TestCart cart(build);
        cart.AddSample({});
        cart.AddSong({Pattern({NoteEntry(0, 60, 0, 64)}, 4)}, 125, 6);
        DriverInfo same;
        std::string error;
        SEQLIB_REQUIRE(DetectDriver(cart.ToRom(), same, error));

        if (build == Build::k2002)
        {
            cart.SetInitRate(16000);
        }
        else
        {
            cart.SetTimerReload(0xFB50);
        }

        DriverInfo other;
        SEQLIB_REQUIRE(DetectDriver(cart.ToRom(), other, error));

        // The test cartridge's game code has no music volume. That gives the one other warning.
        SEQLIB_CHECK_EQ(same.warnings.size(), size_t(1));
        SEQLIB_CHECK_EQ(WarningsWith(same, "music volume"), 1);
        SEQLIB_CHECK_EQ(other.warnings.size(), size_t(2));
        SEQLIB_CHECK_EQ(WarningsWith(other, "pitch and tempo"), 1);
        SEQLIB_CHECK_EQ(WarningsWith(other, build == Build::k2002 ? "16000 Hz" : "1200 cycles"), 1);
        SEQLIB_REQUIRE_EQ(same.song_tables.size(), size_t(1));
        SEQLIB_CHECK(same.song_tables[0].mixer_check == MixerCheck::kSame);
        SEQLIB_CHECK(same.song_tables[0].mixer_warning.empty());
        SEQLIB_REQUIRE_EQ(other.song_tables.size(), size_t(1));
        SEQLIB_CHECK(other.song_tables[0].mixer_check == MixerCheck::kDiffers);
        SEQLIB_CHECK_EQ(WarningsWith(other, other.song_tables[0].mixer_warning), 1);
        SEQLIB_REQUIRE(other.song_tables[0].mixer_warning_index < other.warnings.size());
        SEQLIB_CHECK(other.warnings[other.song_tables[0].mixer_warning_index] == other.song_tables[0].mixer_warning);

        // A 2002 build's rate that detection can't read.
        if (build == Build::k2002)
        {
            cart.SetInitRate(0);
            DriverInfo unread;
            SEQLIB_REQUIRE(DetectDriver(cart.ToRom(), unread, error));
            SEQLIB_REQUIRE_EQ(unread.song_tables.size(), size_t(1));
            SEQLIB_CHECK(unread.song_tables[0].mixer_check == MixerCheck::kUnread);
            SEQLIB_CHECK_EQ(WarningsWith(unread, unread.song_tables[0].mixer_warning), 1);
            SEQLIB_REQUIRE(unread.song_tables[0].mixer_warning_index < unread.warnings.size());
            SEQLIB_CHECK(unread.warnings[unread.song_tables[0].mixer_warning_index] ==
                         unread.song_tables[0].mixer_warning);
        }
    }
}

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));

    return info;
}

void TestRowsAndTicks()
{
    TestCart cart(Build::k2003);
    cart.AddSample({});
    cart.AddSong({Pattern({NoteEntry(0, 60, 0, 64)}, 3), Pattern({}, 2)}, 125, 6);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Player player(rom, info, 0);
    SEQLIB_REQUIRE(player.Valid());
    std::vector<uint64_t> row_times;
    std::vector<int> orders, numbers;
    int ticks = 0;
    player.OnStep(
        [&](bool row)
        {
            ticks++;
            if (row)
            {
                row_times.push_back(player.Elapsed());
                orders.push_back(player.RowOrder());
                numbers.push_back(player.RowNumber());
            }
        });

    // A tick lasts 40000 / 125 = 320 samples, and a row 6 ticks.
    player.Mix(320 * 6 * 6, nullptr);
    SEQLIB_REQUIRE_EQ(row_times.size(), size_t(6));
    SEQLIB_CHECK_EQ(row_times[1], uint64_t(1920));
    SEQLIB_CHECK_EQ(player.RowSamples(), uint32_t(1920));
    // Orders 0 and 1, then the end of the orders goes back to order 0.
    const std::vector<int> expected_orders = {0, 0, 0, 1, 1, 0};
    const std::vector<int> expected_numbers = {0, 1, 2, 0, 1, 0};
    SEQLIB_CHECK(orders == expected_orders);
    SEQLIB_CHECK(numbers == expected_numbers);
    SEQLIB_CHECK_EQ(ticks, 36);
}

void TestNoteAndMix()
{
    TestCart cart(Build::k2003);
    cart.AddSample({});
    cart.AddSong({Pattern({NoteEffectEntry(0, 60, 0, 64, 24, 0)}, 4)}, 125, 6);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Player player(rom, info, 0, kFullMusicVolume);
    std::vector<int8_t> out;
    player.Mix(10, &out);
    const Voice& v = player.Voices()[0];

    // The step: ((16000 * 0x4000) >> 14) * 0x1063 + 0x7FF >> 12.
    SEQLIB_CHECK_EQ(v.speed, uint32_t((16000u * 0x1063u + 0x7FF) >> 12));
    // The levels: 8 * 64 * (64 -/+ 0) >> 10 = 32 on each side, and each sample's byte is 64 * 32 >> 6 = 32.
    SEQLIB_CHECK_EQ(v.left, 32);
    SEQLIB_CHECK_EQ(v.right, 32);
    SEQLIB_REQUIRE_EQ(out.size(), size_t(20));
    SEQLIB_CHECK_EQ(out[0], 32);
    SEQLIB_CHECK_EQ(out[1], 32);
    SEQLIB_CHECK_EQ(v.starts, uint32_t(1));
}

void TestPanAndClip()
{
    // Two channels hard right at full volume: 2 * 64 * 64 >> 6 = 128 on the right, clipped to 127.
    TestCart cart(Build::k2003);
    cart.AddSample({});
    Entry right0 = NoteEntry(0, 60, 0, 64);
    Entry right1 = NoteEntry(1, 60, 0, 64);
    cart.AddSong({Pattern({right0, right1}, 2), Pattern({EffectEntry(0, 24, 64), EffectEntry(1, 24, 64)}, 2)}, 125, 1);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Player player(rom, info, 0, kFullMusicVolume);
    std::vector<int8_t> out;
    // Two rows of one tick each, then the pans.
    player.Mix(320 * 3, &out);
    SEQLIB_CHECK_EQ(player.Voices()[0].left, 0);
    SEQLIB_CHECK_EQ(player.Voices()[0].right, 64);
    SEQLIB_REQUIRE_EQ(out.size(), size_t(320 * 3 * 2));
    SEQLIB_CHECK_EQ(out[2 * 640], 0);
    SEQLIB_CHECK_EQ(out[2 * 640 + 1], 127);
}

void TestByteLevels2002()
{
    // In the 2002 build, each side's sum wraps in 16 bits. A pan past 64 gives a level past a byte, and that level
    // wraps too.
    TestCart cart(Build::k2002);
    cart.AddSample({});
    cart.AddSong({Pattern({NoteEffectEntry(0, 60, 0, 64, 24, 0x80)}, 4)}, 125, 6);
    cart.AddSong({Pattern({}, 1)}, 125, 6);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_REQUIRE(info.song_tables.size() == 1 && info.song_tables[0].build == Build::k2002);

    Player player(rom, info, 0, kFullMusicVolume);
    std::vector<int8_t> out;
    player.Mix(4, &out);
    const Voice& v = player.Voices()[0];
    // pan -128: left 8 * 64 * 192 >> 10 = 96, right 8 * 64 * -64 >> 10 = -32, as a byte 224.
    SEQLIB_CHECK_EQ(v.left, 96);
    SEQLIB_CHECK_EQ(v.right, 224);
    // left: 64 * 96 = 6144 >> 6 = 96, clipped to 127. right: 64 * 224 = 14336 >> 6 = 224, clipped.
    SEQLIB_CHECK_EQ(out[0], 96);
    SEQLIB_CHECK_EQ(out[1], 127);
}

void TestVolumeSlide()
{
    // D04 slides the volume down by 4 at each tick after the row's.
    TestCart cart(Build::k2003);
    cart.AddSample({});
    Entry e = NoteEntry(0, 60, 0, 40);
    e.mask |= kMaskEffect;
    e.effect = 4;
    e.parameter = 0x04;
    cart.AddSong({Pattern({e}, 4)}, 125, 6);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Player player(rom, info, 0);
    player.Mix(320 * 6, nullptr);
    // Ticks 1 to 5 of the row: 40 - 5 * 4.
    SEQLIB_CHECK_EQ(player.Channels()[0].volume, 20);
    SEQLIB_CHECK_EQ(player.Voices()[0].volume, 20);

    // DF2 takes 2 off once, at the row.
    TestCart fine(Build::k2003);
    fine.AddSample({});
    e.parameter = 0xF2;
    fine.AddSong({Pattern({e}, 4)}, 125, 6);
    const Rom fine_rom = fine.ToRom();
    Player fine_player(fine_rom, Detect(fine_rom), 0);
    fine_player.Mix(320 * 6, nullptr);
    SEQLIB_CHECK_EQ(fine_player.Channels()[0].volume, 38);
}

void TestNoteOffAndEnd()
{
    // A note off stops the voice at once. A sample without a loop stops at its end.
    TestCart cart(Build::k2003);
    cart.AddSample({64, 100, 0, 0, 16000});
    cart.AddSample({});
    std::vector<Row> rows(4);
    rows[0] = {NoteEntry(0, 60, 0, 64), NoteEntry(1, 60, 1, 64)};
    rows[2] = {{1, kMaskNote, kNoteOff}};
    cart.AddSong({rows}, 125, 1);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Player player(rom, info, 0);
    player.Mix(50, nullptr);
    SEQLIB_CHECK(player.Voices()[0].flags & kVoiceOn);
    // 100 points at a step of about 1: the sample ends in the first tick's samples.
    player.Mix(270, nullptr);
    SEQLIB_CHECK(!(player.Voices()[0].flags & kVoiceOn));
    SEQLIB_CHECK(!(player.Channels()[0].flags & kChannelSounding));
    SEQLIB_CHECK(player.Voices()[1].flags & kVoiceOn);
    player.Mix(641, nullptr);
    SEQLIB_CHECK(!(player.Voices()[1].flags & kVoiceOn));
}

void TestJumpAndTempo()
{
    // B02 on order 0's first row starts order 2 at the next row; T96 makes a tick 40000 / 150 samples.
    TestCart cart(Build::k2003);
    cart.AddSample({});
    cart.AddSong({Pattern({EffectEntry(0, 2, 2), EffectEntry(1, 20, 150)}, 4), Pattern({}, 4), Pattern({}, 4)}, 125, 1);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Player player(rom, info, 0);
    std::vector<int> orders;
    player.OnStep(
        [&](bool row)
        {
            if (row)
            {
                orders.push_back(player.RowOrder());
            }
        });
    player.Mix(1, nullptr);
    SEQLIB_CHECK_EQ(player.Tempo(), uint32_t(150));
    player.Mix(40000 / 150, nullptr);
    SEQLIB_REQUIRE_EQ(orders.size(), size_t(2));
    SEQLIB_CHECK_EQ(orders[1], 2);
}

void TestPortaAndVibrato()
{
    // G with a note slides toward it at each tick, and stops at it.
    TestCart cart(Build::k2003);
    cart.AddSample({});
    std::vector<Row> rows(2);
    rows[0] = {NoteEntry(0, 60, 0, 64)};
    rows[1] = {{0, kMaskNote | kMaskEffect, 62, 0, 0, 7, 0xFF}};
    cart.AddSong({rows}, 125, 8);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Player player(rom, info, 0);
    player.Mix(320 * 8 + 320 * 3, nullptr);
    const uint32_t target = rom.U32(kSteps + 4 * 62);
    SEQLIB_CHECK_EQ(player.Channels()[0].target, target);
    SEQLIB_CHECK_EQ(player.Channels()[0].step, target);
    SEQLIB_CHECK(!(player.Channels()[0].flags & kChannelTarget));

    // H44 moves the voice's pitch and leaves the channel's step.
    TestCart vib(Build::k2003);
    vib.AddSample({});
    Entry e = NoteEntry(0, 60, 0, 64);
    e.mask |= kMaskEffect;
    e.effect = 8;
    e.parameter = 0x44;
    vib.AddSong({Pattern({e}, 2)}, 125, 6);
    const Rom vib_rom = vib.ToRom();
    Player vib_player(vib_rom, Detect(vib_rom), 0);
    vib_player.Mix(1, nullptr);
    const Channel& ch = vib_player.Channels()[0];
    SEQLIB_CHECK_EQ(ch.vibrato_depth, 16);
    SEQLIB_CHECK_EQ(ch.vibrato_speed, 4);
    SEQLIB_CHECK_EQ(ch.vibrato_place, 4);
    SEQLIB_CHECK_EQ(ch.step, kUnitStep);
    // sin at place 4 is 24 in the test's table: 24 * 16 >> 6 = 6 fine steps up.
    const uint32_t up = vib_rom.U16(kFineUp + 2 * 6);
    SEQLIB_CHECK_EQ(vib_player.Voices()[0].pitch, (kUnitStep * up) >> 15);
}

void TestLoops()
{
    // A forward loop goes back to its start; the place counts in 16384ths of a point.
    TestCart cart(Build::k2003);
    cart.AddSample({64, 100, kSampleLoops, 50, 16000});
    cart.AddSample({64, 100, kSampleLoops | kSamplePingPong, 50, 16000});
    cart.AddSong({Pattern({NoteEntry(0, 60, 0, 64), NoteEntry(1, 60, 1, 64)}, 4)}, 125, 6);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Player player(rom, info, 0);
    player.Mix(1, nullptr); // the tick, and one sample
    const Voice& v = player.Voices()[0];
    const int32_t speed = int32_t(v.speed);
    player.Mix(129, nullptr);
    // 130 steps from 0: past the end at 100 points, the place goes back 50 points each time.
    int32_t expected = 0;
    for (int i = 0; i < 130; i++)
    {
        expected += speed;
        if (expected >= 100 << kFractionBits)
        {
            expected -= 50 << kFractionBits;
        }
    }

    SEQLIB_CHECK_EQ(v.position, expected);

    // The ping-pong voice turns at the end, and plays backwards.
    const Voice& p = player.Voices()[1];
    SEQLIB_CHECK(p.sample_flags & kVoiceBackwards);
    SEQLIB_CHECK_EQ(p.step, -speed);
    // The turn: k steps reach the end, and the place becomes 2 * end - place - k * step - 1 in the 2003 build.
    const int32_t end = 100 << kFractionBits;
    const int32_t k = (end + speed - 1) / speed;
    const int32_t turned = 2 * end - 0 - speed * k - 1;
    SEQLIB_CHECK_EQ(p.position, turned - speed * (130 - k));
}

// The crafted module of webfoot_cart.h plays every effect, loop and mask of the driver. The hashes of each song's first
// 14,336 samples are those of the games' copies of the driver, run on the ARM interpreter with the module in a copy of
// the game's image: Dragon Ball Z: The Legacy of Goku for the 2002 build and The Legacy of Goku II for the later one
// (agblib_check_webfoot_game_driver --crafted).
void TestCraftedModule()
{
    struct Expected
    {
        Build build;
        int volume;
        std::array<uint64_t, 4> hashes;
    };
    const Expected kExpected[] = {
        {Build::k2002, 4, {0xB2007B7344487AFF, 0x49FF36B54F8D7F2B, 0x19F558013253A310, 0x843F5D800321F050}},
        {Build::k2002, 8, {0x0A6B3478C6CE4BFB, 0xC354A1CC5B3AF5ED, 0xD1BC4722D6FBE518, 0xCF776A7EB4FC08E1}},
        {Build::k2003, 4, {0x4A7AA80845F662D2, 0x5E11A94971DD488F, 0xDFDB052994A96599, 0xF4945C7DA48E263B}},
        {Build::k2003, 8, {0x23419D0F9E657039, 0xB6FD26D2D9C9FA53, 0x7A72B0A7F961EF30, 0x7BECC834FCD90C8C}},
    };
    constexpr uint32_t kHashSamples = 14336;

    for (const Expected& e : kExpected)
    {
        TestCart cart(e.build);
        for (const TestSample& s : CraftedSamples())
        {
            cart.AddSample(s);
        }

        for (const TestSong& s : CraftedSongs())
        {
            cart.AddSong(s);
        }

        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        SEQLIB_REQUIRE_EQ(info.SongCount(), int(e.hashes.size()));

        for (size_t song = 0; song < e.hashes.size(); song++)
        {
            Player player(rom, info, int(song), e.volume);
            std::vector<int8_t> out;
            player.Mix(kHashSamples, &out);

            SEQLIB_CHECK_EQ(out.size(), size_t(2 * kHashSamples));
            SEQLIB_CHECK_EQ(Fnv1a(out), e.hashes[song]);
            SEQLIB_CHECK(player.Warnings().empty());
        }
    }
}

int RunTests()
{
    TestDetection();
    TestDetection2002();
    TestMixerCheck();
    TestRowsAndTicks();
    TestNoteAndMix();
    TestPanAndClip();
    TestByteLevels2002();
    TestVolumeSlide();
    TestNoteOffAndEnd();
    TestJumpAndTempo();
    TestPortaAndVibrato();
    TestLoops();
    TestCraftedModule();

    return seqlib::testing::Summary("webfoot");
}

} // namespace
} // namespace agblib::webfoot

int main()
{
    return agblib::webfoot::RunTests();
}
