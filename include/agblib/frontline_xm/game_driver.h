// SPDX-License-Identifier: MIT

// A game's copy of Frontline Studios' XM-like player, run on the GBA's CPU (agb::System). It makes the game's set-up of
// the player, then starts the output and runs the vertical blank's routine once. It starts a song with the player's
// calls that the game makes: the load of the module, the play of the list, the music's switch, the master volume and
// the start of the output. Then each frame it runs the game's frame routine and the vertical blank's routine. The frame
// routine mixes, and the vertical blank's routine swaps the buffers. It's the reference for the model of the player
// (frontline_xm::Player).

#pragma once

#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/frontline_xm/driver.h"

namespace agblib::frontline_xm
{

class GameDriver
{
public:
    // Loads the cartridge and makes the game's set-up. Throws std::runtime_error when a routine fails.
    GameDriver(const Rom& rom, const DriverInfo& info);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Starts song `song` of the driver's list.
    void Start(int song);

    // Runs one frame: the mix, then the vertical blank's swap.
    void Frame();

    // The bytes that the last frame's mix wrote for FIFO A.
    const std::vector<int8_t>& Output() const;

    agb::System& Gba();

private:
    const Rom& rom_;
    const DriverInfo& info_;
    agb::System system_;
    std::vector<int8_t> output_;
};

} // namespace agblib::frontline_xm
