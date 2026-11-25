// SPDX-License-Identifier: MIT

// Unit tests for Engine Software's driver: the bank's reader, the builds, the timing and the model of the driver and
// its mixer on synthetic data. A test reads no game's ROM.

#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"
#include "agblib/engine_software/player.h"
#include "agblib/testing/engine_software_cart.h"
#include "detection_test_util.h"
#include "module_test_util.h"

namespace agblib::engine_software
{
namespace
{

namespace cart = agblib::testing::engine_software_cart;

using agblib::agb::kRomBase;
using agblib::testing::module_tests::OutputHash;

// The build that the model follows.
const Build& SupportedBuild()
{
    return Builds().front();
}

Rom MakeRom()
{
    Rom rom;
    rom.Assign(cart::Image(SupportedBuild()));
    return rom;
}

void TestReadBank()
{
    const Rom rom = MakeRom();

    Bank bank;
    std::vector<Song> songs;
    std::string error;
    const bool read = ReadBank(rom, cart::kBankAddress, bank, songs, error);

    SEQLIB_CHECK(read);
    SEQLIB_CHECK_EQ(bank.kind, cart::kKind);
    SEQLIB_CHECK_EQ(bank.instruments.size(), 4u);
    SEQLIB_CHECK_EQ(bank.instruments[0], cart::kBankAddress + kBankSongTable + 4 * 3);
    SEQLIB_CHECK_EQ(bank.instruments[1], InstrumentEnd(rom, bank.instruments[0]));
    SEQLIB_CHECK_EQ(songs.size(), 3u);
    SEQLIB_CHECK_EQ(songs[0].channels, 4u);
    SEQLIB_CHECK_EQ(songs[0].orders, 4u);
    SEQLIB_CHECK_EQ(songs[0].restart, 1u);
    SEQLIB_CHECK_EQ(songs[0].patterns.size(), 3u);
    SEQLIB_CHECK_EQ(songs[0].tempo, 140u);
    SEQLIB_CHECK_EQ(songs[1].channels, 2u);
    SEQLIB_CHECK_EQ(songs[2].speed, 2u);
    SEQLIB_CHECK(!ReadBank(rom, kRomBase, bank, songs, error));
    SEQLIB_CHECK(!error.empty());
}

void TestNoDriver()
{
    const Rom rom = MakeRom();

    agblib::testing::detection_tests::CheckNoDriver<DriverInfo>(rom, DetectDriver);
}

// The builds: B33E's is followed, BDBE's and BTYE's are known and refused.
void TestBuilds()
{
    const std::vector<Build>& builds = Builds();

    SEQLIB_CHECK_EQ(builds.size(), 3u);
    SEQLIB_CHECK(std::string(builds[0].code) == "B33E");
    SEQLIB_CHECK(builds[0].supported);
    SEQLIB_CHECK(!builds[1].supported);
    SEQLIB_CHECK(!builds[2].supported);
    SEQLIB_CHECK_EQ(builds[0].starts.size(), 12u);
    SEQLIB_CHECK(builds[0].starts[5].jingle);
    SEQLIB_CHECK_EQ(builds[0].starts[6].frames, 1u);
}

// The timing: 224 samples a frame at 13,378 Hz, and the samples a tick at a tempo's 2/5 of a tick a second.
void TestTiming()
{
    const uint32_t frame = FrameSamplesAt(13378);
    const uint32_t tick = TickSamples(13378, 135);
    const uint32_t slow = TickSamples(13378, 1);

    SEQLIB_CHECK_EQ(frame, 224u);
    SEQLIB_CHECK_EQ(StepFactorAt(13378), 0x4E6u);
    SEQLIB_CHECK_EQ(FrameSamplesAt(10512), 176u);
    SEQLIB_CHECK_EQ(FrameSamplesAt(5734), 96u);
    SEQLIB_CHECK_EQ(FrameSamplesAt(12000), 0u);
    SEQLIB_CHECK_EQ(StoredRate(20000), 13378u);
    SEQLIB_CHECK_EQ(tick, 252u);
    SEQLIB_CHECK_EQ(slow, 0u);
}

// The game's steps: the set-up ends with the driver's calls, a song starts with the manager's start and its ramp, and a
// jingle with the music volume, then the manager's jingle.
void TestGameSteps()
{
    const Rom rom = MakeRom();
    const DriverInfo info = cart::TestInfo(rom, SupportedBuild());
    const Build& b = info.build;

    const std::vector<GameStep> set_up = SetUpSteps(info);
    const std::vector<GameStep> song = StartSteps(info, 0);
    const std::vector<GameStep> jingle = StartSteps(info, 1);

    SEQLIB_CHECK_EQ(set_up.size(), size_t(b.manager.block_size / 4 + 8));
    SEQLIB_CHECK_EQ(set_up.front().address, b.manager.object + kManagerBlock);
    SEQLIB_CHECK_EQ(set_up.front().size, 4);
    SEQLIB_CHECK_EQ(set_up.back().address, b.routines.effect_set | 1);
    SEQLIB_CHECK(set_up.back().values == (std::vector<uint32_t>{b.effect_channels, b.effect_table}));
    SEQLIB_CHECK_EQ(song.size(), 1u);
    SEQLIB_CHECK_EQ(song[0].address, b.manager.start | 1);
    SEQLIB_CHECK(song[0].values == (std::vector<uint32_t>{b.manager.object, 0, 0x40, 0xD2}));
    SEQLIB_CHECK_EQ(jingle.size(), 2u);
    SEQLIB_CHECK_EQ(jingle[0].address, b.routines.music_volume | 1);
    SEQLIB_CHECK(jingle[0].values == (std::vector<uint32_t>{0xD2}));
    SEQLIB_CHECK_EQ(jingle[1].address, b.manager.jingle | 1);
    SEQLIB_CHECK(jingle[1].values == (std::vector<uint32_t>{b.manager.object, 1}));
}

// A song's start ramps the music volume up from 0 in steps of 4 a frame toward the manager's 0xD2. A ramp of 1 frame
// reaches it at once.
void TestRamp()
{
    const Rom rom = MakeRom();
    const DriverInfo info = cart::TestInfo(rom, SupportedBuild());
    Player ramped(rom, info);
    Player direct(rom, info);
    ramped.Start(0);
    direct.Start(2);

    const uint16_t before = ramped.MusicVolume();
    ramped.Frame();
    const uint16_t first = ramped.MusicVolume();
    ramped.Frame();
    direct.Frame();

    SEQLIB_CHECK_EQ(before, 0u);
    SEQLIB_CHECK_EQ(first, 4u);
    SEQLIB_CHECK_EQ(ramped.MusicVolume(), 8u);
    SEQLIB_CHECK_EQ(direct.MusicVolume(), 0xD2u);
}

// A jingle plays once at the manager's volume, then stops.
void TestJingleEnds()
{
    const Rom rom = MakeRom();
    const DriverInfo info = cart::TestInfo(rom, SupportedBuild());
    Player player(rom, info);
    player.Start(1);

    const uint16_t volume = player.MusicVolume();
    int frames = 0;
    while (player.Playing() && frames < 1000)
    {
        player.Frame();
        frames++;
    }

    SEQLIB_CHECK_EQ(volume, 0xD2u);
    SEQLIB_CHECK(!player.Playing());
    SEQLIB_CHECK(frames < 1000);
    SEQLIB_CHECK_EQ(player.Unmodelled(), 0);
}

// The model's bytes for FIFO A in the songs of the test cartridge, against the hashes of the game's code: the driver of
// The Santa Clause 3 running the same bank (agblib_check_engine_software_game_driver --crafted).
void TestOutputHashes()
{
    const Rom rom = MakeRom();
    const DriverInfo info = cart::TestInfo(rom, SupportedBuild());
    const uint64_t expected[] = {0x9339434D815EC761ull, 0x71E5597838FDB218ull, 0x254C11E56EC08AABull};

    for (int song = 0; song < 3; song++)
    {
        Player player(rom, info);
        player.Start(song);
        const uint64_t hash = OutputHash(player, 1500);

        SEQLIB_CHECK_EQ(hash, expected[song]);
        SEQLIB_CHECK_EQ(player.Unmodelled(), 0);
    }
}

int RunTests()
{
    TestReadBank();
    TestNoDriver();
    TestBuilds();
    TestTiming();
    TestGameSteps();
    TestRamp();
    TestJingleEnds();
    TestOutputHashes();

    return seqlib::testing::Summary("engine_software");
}

} // namespace
} // namespace agblib::engine_software

int main()
{
    return agblib::engine_software::RunTests();
}
