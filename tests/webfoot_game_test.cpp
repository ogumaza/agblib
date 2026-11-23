// SPDX-License-Identifier: MIT

// Tests of the parts of webfoot::GameDriver that a stub shares (agblib/webfoot/game_driver.h): the search for the
// game's master interrupt handler, and the call of the driver's handler as the master handler makes it. The driver's
// handlers here are short programs whose instructions come with their assembly.

#include <cstdint>

#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/webfoot_cart.h"
#include "agblib/webfoot/driver.h"
#include "agblib/webfoot/game_driver.h"

namespace agblib::webfoot
{
namespace
{

using agblib::agb::kIwramBase;
using agblib::agb::kRomBase;
using agblib::testing::agb_files::CartImage;
using agblib::testing::webfoot_cart::PutMasterHandler;

constexpr uint32_t kImageSize = 0x4000;

// The word that the test's driver handlers store r3 to.
constexpr uint32_t kSeenRegister = kIwramBase + 0x1000;

// The I/O base that the master handler leaves in r3.
constexpr uint32_t kIoBase = 0x04000200;

void TestMasterHandler2002()
{
    // The 2002 build's master handler is in the cartridge, after its program's start.
    CartImage image(kImageSize);
    PutMasterHandler(image, kRomBase + 0x138);
    GameRoutines routines;
    routines.build = Build::k2002;
    routines.program = kRomBase;

    const uint32_t found = FindMasterHandler(image.ToRom(), routines);

    SEQLIB_CHECK_EQ(found, kRomBase + 0x138);
}

void TestMasterHandler2003()
{
    // The later build's master handler is in the image of IWRAM that the program's crt0 copies.
    CartImage image(kImageSize);
    PutMasterHandler(image, kRomBase + 0x2000 + 0x824);
    GameRoutines routines;
    routines.program = kRomBase;
    routines.iwram_source = kRomBase + 0x2000;
    routines.iwram_size = 0x1000;

    const uint32_t found = FindMasterHandler(image.ToRom(), routines);

    SEQLIB_CHECK_EQ(found, kIwramBase + 0x824);
}

void TestNoMasterHandler()
{
    // A 2002 build's handler past the reach, a later build's handler outside its image, and four of the five
    // instructions are no master handler.
    CartImage far(kImageSize);
    PutMasterHandler(far, kRomBase + kMasterHandlerReach);
    GameRoutines rom_routines;
    rom_routines.build = Build::k2002;
    rom_routines.program = kRomBase;

    CartImage outside(kImageSize);
    PutMasterHandler(outside, kRomBase + 0x1000);
    GameRoutines iwram_routines;
    iwram_routines.iwram_source = kRomBase + 0x2000;
    iwram_routines.iwram_size = 0x1000;

    CartImage partial(kImageSize);
    PutMasterHandler(partial, kRomBase + 0x138);
    partial.Put32(kRomBase + 0x138 + 16, 0);

    SEQLIB_CHECK_EQ(FindMasterHandler(far.ToRom(), rom_routines), 0u);
    SEQLIB_CHECK_EQ(FindMasterHandler(outside.ToRom(), iwram_routines), 0u);
    SEQLIB_CHECK_EQ(FindMasterHandler(partial.ToRom(), rom_routines), 0u);
}

void TestCallHandler2002()
{
    // A Thumb handler that stores r3 and returns through r6, as the 2002 build's does.
    CartImage image(kImageSize);
    constexpr uint32_t kHandler = kRomBase + 0x200;
    image.Put16(kHandler, 0x4801);     // ldr r0, [pc, #4]
    image.Put16(kHandler + 2, 0x6003); // str r3, [r0]
    image.Put16(kHandler + 4, 0x4730); // bx r6
    image.Put32(kHandler + 8, kSeenRegister);
    const agb::Rom rom = image.ToRom();
    agb::System gba(rom);
    GameRoutines routines;
    routines.build = Build::k2002;
    routines.handler = kHandler | 1;
    routines.return_register = 6;

    CallHandler(gba, routines);

    SEQLIB_CHECK_EQ(gba.Load(kSeenRegister, 4), kIoBase);
}

void TestCallHandler2003()
{
    // An ARM handler that stores r3 and returns through r4, as the later build's does.
    CartImage image(kImageSize);
    constexpr uint32_t kHandler = kRomBase + 0x200;
    image.Put32(kHandler, 0xE59F0004);     // ldr r0, [pc, #4]
    image.Put32(kHandler + 4, 0xE5803000); // str r3, [r0]
    image.Put32(kHandler + 8, 0xE12FFF14); // bx r4
    image.Put32(kHandler + 12, kSeenRegister);
    const agb::Rom rom = image.ToRom();
    agb::System gba(rom);
    GameRoutines routines;
    routines.handler = kHandler;
    routines.return_register = 4;

    CallHandler(gba, routines);

    SEQLIB_CHECK_EQ(gba.Load(kSeenRegister, 4), kIoBase);
}

int RunTests()
{
    TestMasterHandler2002();
    TestMasterHandler2003();
    TestNoMasterHandler();
    TestCallHandler2002();
    TestCallHandler2003();

    return seqlib::testing::Summary("webfoot_game");
}

} // namespace
} // namespace agblib::webfoot

int main()
{
    return agblib::webfoot::RunTests();
}
