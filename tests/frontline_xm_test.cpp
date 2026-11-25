// SPDX-License-Identifier: MIT

// Unit tests for Frontline Studios' XM-like player: the module's reader and the model of the player and the mixer on
// synthetic data. A test reads no game's ROM.

#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"
#include "agblib/frontline_xm/player.h"
#include "agblib/testing/frontline_xm_cart.h"
#include "detection_test_util.h"
#include "frontline_xm_test_util.h"
#include "module_test_util.h"

namespace agblib::frontline_xm
{
namespace
{

namespace cart = agblib::testing::frontline_xm_cart;

using agblib::agb::kRomBase;
using agblib::testing::frontline_xm_tests::TestDriver;
using agblib::testing::module_tests::OutputHash;

// A cartridge that holds the synthetic data at its start.
Rom MakeRom(const cart::Data& data)
{
    Rom rom;
    rom.Assign(data.Bytes());
    return rom;
}

// The driver as the model reads it: the data's tables, its module, and its two lists.
DriverInfo MakeInfo(const Rom& rom, const cart::Data& data)
{
    DriverInfo info = TestDriver();
    Module module;
    ReadModule(rom, data.Module(), module);
    info.modules = {module};
    info.songs = {{0, data.Orders(), data.LastOrder(), cart::kFirstVolume},
                  {0, data.SecondList(), data.SecondLast(), cart::kSecondVolume}};
    return info;
}

void TestReadModule()
{
    const cart::Data data(kRomBase);
    const Rom rom = MakeRom(data);

    Module module;
    const bool read = ReadModule(rom, data.Module(), module);

    SEQLIB_CHECK(read);
    SEQLIB_CHECK_EQ(module.channels, 4u);
    SEQLIB_CHECK_EQ(module.last_order, data.LastOrder());
    SEQLIB_CHECK_EQ(module.speed, cart::kSpeed);
    SEQLIB_CHECK_EQ(module.bpm, cart::kBpm);
    SEQLIB_CHECK(!ReadModule(rom, kRomBase + cart::kSteps, module));
}

void TestNoDriver()
{
    const cart::Data data(kRomBase);
    const Rom rom = MakeRom(data);

    agblib::testing::detection_tests::CheckNoDriver<DriverInfo>(rom, DetectDriver);
}

// The samples a tick at the module's BPM: the rate x 5 / (2 x BPM).
void TestTiming()
{
    const cart::Data data(kRomBase);
    const Rom rom = MakeRom(data);
    const DriverInfo info = MakeInfo(rom, data);
    Player player(rom, info);

    player.Start(0);

    SEQLIB_CHECK_EQ(player.FrameSamples(), uint32_t(cart::kFrameSampleCount));
    SEQLIB_CHECK_EQ(player.TickSamples(), uint16_t(cart::kRate * 5 / (2 * cart::kBpm)));
    SEQLIB_CHECK_EQ(player.Master(), cart::kFirstVolume);
}

// The bytes of the mixer for 1,500 frames of each list. The players and mixers of Chicken Shoot and Chicken Shoot 2,
// on the ARM interpreter with the data's tables in place of the games' tables, gave the same bytes.
void TestOutput()
{
    const cart::Data data(kRomBase);
    const Rom rom = MakeRom(data);
    const DriverInfo info = MakeInfo(rom, data);

    Player first(rom, info);
    first.Start(0);
    Player second(rom, info);
    second.Start(1);

    SEQLIB_CHECK_EQ(OutputHash(first, 1500), 0xF00C933632CF1999ull);
    SEQLIB_CHECK_EQ(OutputHash(second, 1500), 0x7EDA6E5871BFDEC6ull);
    SEQLIB_CHECK_EQ(first.Unmodelled(), 0);
    SEQLIB_CHECK_EQ(second.Unmodelled(), 0);
}

// A delayed note sounds its pitch without setting the channel's period. The next row without a note goes back to the
// period of the note before (pattern 0, channel 1: the note 50 of row 8, then the note 52 that row 9 delays).
void TestDelayedNote()
{
    const cart::Data data(kRomBase);
    const Rom rom = MakeRom(data);
    const DriverInfo info = MakeInfo(rom, data);
    Player player(rom, info);
    player.Start(0);

    // Period = 7680 - 64 x note - finetune / 2, with the noise's finetune of -20.
    constexpr uint16_t kRowEight = 7680 - 64 * 50 + 10;
    constexpr uint16_t kDelayed = 7680 - 64 * 52 + 10;
    uint16_t before_row_ten = 0;
    uint16_t period_before_row_ten = 0;
    uint16_t after_row_ten = 0;
    player.on_tick = [&]()
    {
        const Channel& c = player.ChannelAt(1);
        if (player.Order() == 0 && player.Row() == 10 && player.TickCount() == player.Speed())
        {
            before_row_ten = c.sounding;
            period_before_row_ten = c.period;
        }

        if (player.Order() == 0 && player.Row() == 11 && player.TickCount() == 1 && after_row_ten == 0)
        {
            after_row_ten = c.sounding;
        }
    };
    for (int frame = 0; frame < 200 && after_row_ten == 0; frame++)
    {
        player.Frame();
    }

    SEQLIB_CHECK_EQ(before_row_ten, kDelayed);
    SEQLIB_CHECK_EQ(period_before_row_ten, kRowEight);
    SEQLIB_CHECK_EQ(after_row_ten, kRowEight);
}

} // namespace

int RunTests()
{
    TestReadModule();
    TestNoDriver();
    TestTiming();
    TestOutput();
    TestDelayedNote();

    return seqlib::testing::Summary("frontline_xm");
}

} // namespace agblib::frontline_xm

int main()
{
    return agblib::frontline_xm::RunTests();
}
