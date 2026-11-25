// SPDX-License-Identifier: MIT

// Unit tests for Frontline Studios' MOD player: the module's reader, the parts of a module, and the model of the
// player and the mixer on synthetic data. A test reads no game's ROM.

#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/frontline_mod/driver.h"
#include "agblib/frontline_mod/player.h"
#include "agblib/testing/frontline_mod_cart.h"
#include "detection_test_util.h"
#include "module_test_util.h"

namespace agblib::frontline_mod
{
namespace
{

namespace cart = agblib::testing::frontline_mod_cart;

using agblib::agb::kRomBase;
using agblib::testing::module_tests::OutputHash;

constexpr uint32_t kIwramBase = 0x03000000;

// A cartridge that holds the synthetic data at its start.
Rom MakeRom()
{
    const cart::Data data(kRomBase);
    Rom rom;
    rom.Assign(data.Bytes());
    return rom;
}

// The driver as the model reads it: the data's tables, and the mixer that Hardcore Pool sets up.
DriverInfo MakeInfo()
{
    DriverInfo info;
    info.periods = kRomBase + cart::kPeriods;
    info.vibrato = kIwramBase;
    info.image_base = kRomBase + cart::kVibrato;
    info.copy_start = kIwramBase;
    info.copy_end = kIwramBase + 64;
    return info;
}

void TestReadModule()
{
    const Rom rom = MakeRom();

    Module module;
    const bool read = ReadModule(rom, kRomBase + cart::kModule, module);

    SEQLIB_CHECK(read);
    SEQLIB_CHECK_EQ(module.channels, 4u);
    SEQLIB_CHECK_EQ(module.order_count, cart::kOrderCount);
    SEQLIB_CHECK_EQ(module.speed, cart::kSpeed);
    SEQLIB_CHECK(!ReadModule(rom, kRomBase + cart::kSamples, module));
}

void TestFindJump()
{
    const Rom rom = MakeRom();
    Module module;
    SEQLIB_CHECK(ReadModule(rom, kRomBase + cart::kModule, module));

    uint32_t target = 0;
    const int jump = FindJump(rom, module, 0, target);
    uint32_t none = 0;
    const int after = FindJump(rom, module, 1, none);

    SEQLIB_CHECK_EQ(jump, 0);
    SEQLIB_CHECK_EQ(target, 1u);
    SEQLIB_CHECK_EQ(after, -1);
}

void TestNoDriver()
{
    const Rom rom = MakeRom();

    agblib::testing::detection_tests::CheckNoDriver<DriverInfo>(rom, DetectDriver);
}

// The bytes of the mixer for 1,200 frames from each start. The game's player and mixer (Hardcore Pool's, on the ARM
// interpreter, with the data's tables in place of the game's) gave the same bytes.
void TestOutput()
{
    const Rom rom = MakeRom();
    const DriverInfo info = MakeInfo();
    Module module;
    SEQLIB_CHECK(ReadModule(rom, kRomBase + cart::kModule, module));

    Player first(rom, info);
    first.Start(module, 0, kFullVolume);
    Player second(rom, info);
    second.Start(module, 2, 31);

    SEQLIB_CHECK_EQ(OutputHash(first, 1200), 0x805EC460601D56A4ull);
    SEQLIB_CHECK_EQ(OutputHash(second, 1200), 0x2D521208AC750340ull);
    SEQLIB_CHECK_EQ(first.Unmodelled(), 0u);
    SEQLIB_CHECK_EQ(second.Unmodelled(), 0u);
}

// A start with a step of 31 raises the music's volume by 31 at each row.
void TestFade()
{
    const Rom rom = MakeRom();
    const DriverInfo info = MakeInfo();
    Module module;
    SEQLIB_CHECK(ReadModule(rom, kRomBase + cart::kModule, module));
    Player player(rom, info);
    player.Start(module, 2, 31);

    std::vector<int32_t> volumes;
    for (int frame = 0; frame < 3 * int(cart::kSpeed); frame++)
    {
        if (player.Play())
        {
            volumes.push_back(player.MusicVolume());
        }
    }

    SEQLIB_CHECK_EQ(volumes.size(), size_t(3));
    SEQLIB_CHECK_EQ(volumes[0], 31);
    SEQLIB_CHECK_EQ(volumes[2], 93);
}

} // namespace

int RunTests()
{
    TestReadModule();
    TestFindJump();
    TestNoDriver();
    TestOutput();
    TestFade();

    return seqlib::testing::Summary("frontline_mod");
}

} // namespace agblib::frontline_mod

int main()
{
    return agblib::frontline_mod::RunTests();
}
