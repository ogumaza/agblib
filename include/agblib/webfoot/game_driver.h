// SPDX-License-Identifier: MIT

// A game's copy of Webfoot Technologies' driver, run on the GBA's CPU (agb::System). It calls the routines that the
// game calls: the driver's init, the setter of the music volume and the start of a song. Then it calls the driver's
// interrupt handler as the console's sound DMA or timer would. The handler counts the samples that the FIFOs have
// played and runs the mix once the mixer's lead falls to its mark. It's the reference for the model of the driver
// (webfoot::Player).
//
// The handlers don't return like routines. They return through a register that the game's master handler sets (r6 in
// the 2002 build, r4 in the later one), and they read the I/O base 0x04000200 from r3. CallHandler() sets both. When
// the handler mixes, it turns the interrupts on and jumps to the mix routine. Nothing interrupts the call here.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/webfoot/driver.h"

namespace agblib::webfoot
{

// The routines of a program's copy of the driver, and what calling them requires.
struct GameRoutines
{
    Build build = Build::k2003;
    uint32_t program = 0;      // the program's start in the cartridge
    uint32_t iwram_source = 0; // the ROM's image of IWRAM that the program's crt0 copies to 0x03000000, or 0
    uint32_t iwram_size = 0;
    uint32_t init = 0;          // Thumb: r0 = init_argument
    uint32_t init_argument = 0; // the 2002 build: the mixer's nominal rate; the later one: the driver's object
    uint32_t music_volume = 0;  // Thumb: the 2002 build, r0 = the volume; the later one, r0 = player, r1 = the volume
    uint32_t play = 0;       // Thumb: the 2002 build, r0 = the song's header; the later one, r0 = player, r1 = header
    uint32_t player = 0;     // the music player's record in the later build, or 0
    uint32_t handler = 0;    // the driver's interrupt handler, with bit 0 set for Thumb code
    uint32_t positions = 0;  // the word of the FIFOs' place in the ring; the mixer's place in the ring follows it
    int return_register = 0; // the register that the handler returns through
    uint32_t samples_per_interrupt = 0;
    uint32_t master_handler = 0; // the game's master interrupt handler (ARM code), or 0 where it wasn't found
};

// Finds the routines of the copy of the driver that plays `table`'s songs, by their code. Returns false and sets
// `error` to what wasn't found. A game's master handler that FindMasterHandler() doesn't find leaves
// `master_handler` at 0 and isn't an error.
bool FindGameRoutines(const Rom& rom, const SongTable& table, GameRoutines& routines, std::string& error);

// How far after the start of its program the 2002 build's master interrupt handler can be.
inline constexpr uint32_t kMasterHandlerReach = 0x1000;

// Finds the game's master interrupt handler for the copy of the driver of `routines` by its first five instructions.
// The master handler acknowledges the interrupt, sets r3 to 0x04000200 and the register that the driver's handler
// returns through, and calls the routine of the game's table of handlers. The later build's is in the program's image
// of IWRAM, and the 2002 build's is in the cartridge within kMasterHandlerReach of the program's start. Returns its
// address, or 0.
uint32_t FindMasterHandler(const Rom& rom, const GameRoutines& routines);

// Runs the driver's interrupt handler once on `gba` as the game's master handler calls it: with the I/O base
// 0x04000200 in r3, and with the register that the handler returns through set to the call's return.
void CallHandler(agb::System& gba, const GameRoutines& routines);

class GameDriver
{
public:
    // The bytes of a ring of each side, and the ring's place that the DMA of the right side restarts at.
    static constexpr uint32_t kRingSize = 0x400;
    static constexpr uint32_t kRightRestart = 0x200;

    // Loads the cartridge, copies the program's image of IWRAM and runs the driver's init. Throws std::runtime_error
    // when a routine fails.
    GameDriver(const Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Calls the game's setter of the music volume, 0 to kFullMusicVolume.
    void SetMusicVolume(int volume);

    // Starts the song whose header is at `header`.
    void Play(uint32_t header);

    // Runs the driver's interrupt handler once (CallHandler()). Returns true when the handler mixed.
    bool Interrupt();

    // The bytes of the last mix in the order that the FIFOs play them: each sample's left byte, then its right byte.
    const std::vector<int8_t>& Output() const;

    // The timer 0 reload and the interrupts that the init enabled.
    uint16_t TimerReload();
    uint16_t InterruptsEnabled();

    agb::System& Gba();

private:
    // The ring's place of the FIFOs and of the mixer.
    uint32_t PlayPlace();
    uint32_t MixPlace();

    GameRoutines routines_;
    agb::System system_;
    uint32_t left_ring_ = 0;
    uint32_t right_ring_ = 0;
    std::vector<int8_t> output_;
};

} // namespace agblib::webfoot
