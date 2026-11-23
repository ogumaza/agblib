// SPDX-License-Identifier: MIT

// Tests of the reference that runs a game's driver (agblib/konami/game_driver.h). Small programs stand in for a
// game's routines. They build small synthetic cartridges in memory. No game ROM is needed.

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "xsflib/testing/check.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/konami/driver.h"
#include "agblib/konami/game_driver.h"
#include "agblib/konami/output.h"
#include "agblib/konami/seqformat.h"
#include "agblib/konami/sequencer.h"
#include "konami_output_image.h"

namespace
{

using namespace konami_testing;

using agblib::agb::kRomBase;
using agblib::agb::Rom;
using agblib::konami::GameDriver;
using agblib::konami::GameRoutines;

// The reference. Small programs stand in for a game's routines.
constexpr uint32_t kReturn = kRomBase + 0x100; // bx lr
constexpr uint32_t kFrameEntry = kRomBase + 0x110;
constexpr uint32_t kStage = kRomBase + 0x130;
constexpr uint32_t kArmMixer = kRomBase + 0x150;
constexpr uint32_t kFifoIrq = kRomBase + 0x170;
constexpr uint32_t kMixerBuffer = 0x03001000;
constexpr uint32_t kDmaBuffer = 0x03002000;

Rom ReferenceRom()
{
    agblib::testing::agb_files::CartImage m(0x1000);
    m.PutHalfwords(kReturn, {0x4770});

    // push {lr} / sub sp, #40 / movs r0, #0x55 / add r1, sp, #8 / strb r0, [r1, #3], then a call of the output stage /
    // add sp, #40 / pop {r0} / bx r0
    m.PutHalfwords(kFrameEntry, {0xB500, 0xB08A, 0x2055, 0xA902, 0x70C8});
    m.PutBl(kFrameEntry + 10, kStage);
    m.PutHalfwords(kFrameEntry + 14, {0xB00A, 0xBC01, 0x4700});

    // The output stage stores FIFO A's reset bit, and stores what the register then reads as in NR51.
    // ldr r3, =SOUNDCNT_H's high byte / movs r0, #0x3B / strb r0, [r3] / ldrb r1, [r3] / ldr r3, =NR51 / strb r1, [r3]
    // / bx lr / nop
    m.PutHalfwords(kStage, {0x4B03, 0x203B, 0x7018, 0x7819, 0x4B02, 0x7019, 0x4770, 0x46C0});
    m.Put32(kStage + 16, kRegSoundCntHHigh);
    m.Put32(kStage + 20, kRegNr51);

    // The mixer, in ARM: it counts in the first byte of its buffer. ldr r0, [pc, #12] / ldrb r1, [r0] /
    // add r1, r1, #1 / strb r1, [r0] / bx lr
    m.Put32(kArmMixer, 0xE59F000C);
    m.Put32(kArmMixer + 4, 0xE5D01000);
    m.Put32(kArmMixer + 8, 0xE2811001);
    m.Put32(kArmMixer + 12, 0xE5C01000);
    m.Put32(kArmMixer + 16, 0xE12FFF1E);
    m.Put32(kArmMixer + 20, kMixerBuffer);

    // A DMA's interrupt routine counts in the first byte of the DMA's source.
    // ldr r0, =source / ldrb r1, [r0] / adds r1, #1 / strb r1, [r0] / bx lr
    m.PutHalfwords(kFifoIrq, {0x4803, 0x7801, 0x3101, 0x7001, 0x4770});
    m.Put32(kFifoIrq + 16, kDmaBuffer);
    return m.ToRom();
}

GameRoutines ReferenceRoutines()
{
    GameRoutines routines;
    routines.init = kReturn;
    routines.request_song = kReturn;
    routines.start_song = kReturn;
    routines.frame = kFrameEntry;
    routines.output_stage = kStage;
    routines.tracks = 2;
    routines.fifo_b = 0x20;
    return routines;
}

void TestReferenceFrame()
{
    const Rom rom = ReferenceRom();
    GameRoutines routines = ReferenceRoutines();
    routines.mixer = kArmMixer;
    routines.fifo_buffer = kMixerBuffer;
    GameDriver driver(rom, routines);
    driver.Play(0);

    // Timer 0's reload gives 0x400 cycles a point, and a block of 16 points 0x4000 cycles.
    driver.Gba().Store(agblib::agb::kRegTimer0Reload, 2, 0x10000 - 0x400);
    driver.Gba().Store(agblib::agb::kRegTimer0Reload + 2, 2, 0x80);
    XSFLIB_CHECK(driver.BlockCycles() == 0x4000);
    XSFLIB_CHECK(driver.Rate() == 16384.0);

    // The records lie 8 bytes above the stack pointer at the output stage. A store with FIFO A's reset bit reads back
    // without it, and the other bits stay.
    XSFLIB_CHECK(driver.Frame());
    XSFLIB_CHECK(driver.Records().size() == 2 && driver.Records()[0].vol == 0x55);
    XSFLIB_CHECK(SameWrites(driver.SoundWrites(), {{kRegSoundCntHHigh, 1, 0x3B}, {kRegNr51, 1, 0x33}}));

    // A store of the whole register loses both FIFOs' reset bits.
    driver.Gba().Store(kRegSoundCntH, 2, 0xBB0E);
    XSFLIB_CHECK(driver.Gba().Load(kRegSoundCntH, 2) == 0x330E);

    // A frame holds 17 blocks and 0x940 cycles. The cycles left over reach a whole block in the 7th frame: 18 blocks.
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    int mixed = 0;
    for (int f = 0; f < 7; f++)
    {
        if (f != 0)
        {
            driver.Frame();
        }

        driver.Mix(a, b);
        XSFLIB_CHECK(a.size() == (f == 6 ? 18u : 17u) * 16);
        XSFLIB_CHECK(b.size() == a.size());
        mixed += int(a.size() / 16);
        XSFLIB_CHECK(a[a.size() - 16] == int8_t(mixed));
    }
}

void TestReferenceFifo()
{
    const Rom rom = ReferenceRom();
    GameRoutines routines = ReferenceRoutines();
    routines.fifo_irqs = {kFifoIrq, kFifoIrq};
    GameDriver driver(rom, routines);
    driver.Play(0);

    // Timer 0 runs at 0x100 cycles a sample. DMA 1 takes FIFO A's bytes from the buffer. Its timing bits keep it from
    // starting at the store. DMA 2 stays off. FIFO B takes nothing.
    driver.Gba().Store(agblib::agb::kRegTimer0Reload, 2, 0x10000 - 0x100);
    driver.Gba().Store(agblib::agb::kRegTimer0Reload + 2, 2, 0x80);
    driver.Gba().Store(0x040000BC, 4, kDmaBuffer);
    driver.Gba().Store(0x040000C6, 2, 0xB640);

    // An empty FIFO takes 16 bytes at the first sample, and has 15 at the next: 16 more. It then takes 16 bytes for
    // each 16 samples. The frame holds 1,097 samples of 0x100 cycles. Their DMAs come at samples 1, 2, 18, 34 and so on
    // up to 1,090: 70.
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    driver.Frame();
    driver.Mix(a, b);
    XSFLIB_CHECK(a.size() == 70 * 16 && b.empty());
    XSFLIB_CHECK(a[0] == 0 && a[16] == 1 && a[32] == 2 && a[17] == 0);

    // The output stage's store of the reset bit empties the FIFO. The next frame's first samples behave as before. Its
    // first sample comes 192 cycles into the frame, and it holds 1,097 again.
    driver.Frame();
    driver.Mix(a, b);
    XSFLIB_CHECK(a.size() == 70 * 16);
}

} // namespace

int main()
{
    TestReferenceFrame();
    TestReferenceFifo();

    return xsflib::testing::Summary("the reference of Konami's driver");
}
