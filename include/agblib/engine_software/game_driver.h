// SPDX-License-Identifier: MIT

// A game's copy of Engine Software's driver, run on the GBA's CPU (agb::System), with the game's sound manager. It
// places the manager's object and its block of fields in EWRAM, then makes the manager's set-up without its
// allocation: the music and effect volumes, then the driver's reset, init, bank load and effect set with the game's
// values. It starts a song with the manager's start and its ramp of the music volume up from 0, or plays a jingle at
// the manager's music volume. Each frame runs the manager's routine of the vertical count, then its routine of the
// vertical blank. The first takes a step of the ramps and runs the frame routine, and the second swaps the buffers.
// It's the reference for the model of the driver (engine_software::Player).

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/engine_software/driver.h"

namespace agblib::engine_software
{

class GameDriver
{
public:
    // Loads the cartridge and makes the game's set-up. Throws std::runtime_error when a routine fails.
    GameDriver(const Rom& rom, const DriverInfo& info);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Starts song `song` of `info.songs` as the game does.
    void Start(int song);

    // Runs one frame: the routine of the vertical count, then the vertical blank's.
    void Frame();

    // The bytes that the last frame's mix wrote for FIFO A.
    const std::vector<int8_t>& Output() const;

    agb::System& Gba();

private:
    const DriverInfo& info_;
    agb::System system_;
    std::vector<int8_t> output_;
};

// Makes each of `steps` on `system`, in order.
void RunSteps(agb::System& system, std::span<const GameStep> steps);

// Makes the game's set-up of the manager and the driver on `system` (SetUpSteps()), with the manager's object at
// `info.build.manager.object`.
void SetUpManager(agb::System& system, const DriverInfo& info);

} // namespace agblib::engine_software
