// SPDX-License-Identifier: MIT

#include "agblib/frontline_xm/game_driver.h"

#include <array>
#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/frontline_xm/driver.h"

namespace agblib::frontline_xm
{
namespace
{

// The load takes the module's header less 4: the word before the header.
constexpr uint32_t kLoadOffset = 4;

} // namespace

GameDriver::GameDriver(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info), system_(rom)
{
    // The set-up leaves the output stopped. The game starts it after it starts a song, and the next vertical blank sets
    // the samples a frame for the rate. Here that happens once before any song. Each song therefore starts with the
    // frames of the rate as the songs after the game's first do. The game's first song, the title at boot, mixes its
    // first frame at the player's initial count instead, 224 samples.
    system_.Call(info_.sound_init);
    system_.Call(info_.library.output);
    system_.Call(info_.library.swap);
}

void GameDriver::Start(int song)
{
    const Song& s = info_.songs[size_t(song)];
    const Library& lib = info_.library;
    system_.Call(lib.load, std::array<uint32_t, 2>{info_.modules[size_t(s.module)].address - kLoadOffset, 0});
    system_.Call(lib.play, std::array<uint32_t, 3>{s.list, 0, s.last});
    system_.Call(lib.music, std::array<uint32_t, 1>{1});
    system_.Call(lib.volume, std::array<uint32_t, 1>{s.volume});
    system_.Call(lib.output);
}

void GameDriver::Frame()
{
    const uint32_t buffer = system_.Load(info_.library.state + kStateOutput, 4);
    system_.Call(info_.library.frame);
    const uint32_t samples = info_.FrameSamples(rom_);
    output_.resize(samples);
    for (uint32_t i = 0; i < samples; i++)
    {
        output_[i] = int8_t(system_.Load(buffer + i, 1));
    }

    system_.Call(info_.library.swap);
}

const std::vector<int8_t>& GameDriver::Output() const
{
    return output_;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace agblib::frontline_xm
