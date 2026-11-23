// SPDX-License-Identifier: MIT

// A game's copy of Frontline Studios' MOD player, run on the GBA's CPU (agb::System). It copies the game's image of
// IWRAM, makes the game's set-up of the driver and starts a module at an order as the game does. Then each frame it
// runs the player and the mixer as the game's vertical blank does. It's the reference for the model of the player
// (frontline_mod::Player).
//
// The vertical blank first restarts the sound DMA on the buffer that the last frame filled. That changes nothing that
// the mixer writes, and the driver here leaves it out.

#pragma once

#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/frontline_mod/driver.h"

namespace agblib::frontline_mod
{

class GameDriver
{
public:
    // Loads the cartridge, copies the image of IWRAM and makes the game's set-up. Throws std::runtime_error when a
    // routine fails.
    GameDriver(const Rom& rom, const DriverInfo& info);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Starts the module at `module` at order `order` through the game's start, with the fade step `step`.
    void Start(uint32_t module, uint32_t order, int32_t step);

    // Runs the player and the mixer once.
    void Frame();

    // The bytes that the last frame's mix wrote for FIFO A.
    const std::vector<int8_t>& Output() const;

    agb::System& Gba();

private:
    const DriverInfo& info_;
    agb::System system_;
    std::vector<int8_t> output_;
};

} // namespace agblib::frontline_mod
