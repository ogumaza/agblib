// SPDX-License-Identifier: MIT

#include "agblib/frontline_mod/game_driver.h"

#include <array>
#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/frontline_mod/driver.h"

namespace agblib::frontline_mod
{
namespace
{

using agblib::agb::kIwramBase;

// The mixer's context: the word of the buffer that it filled last as an offset from the first buffer.
constexpr uint32_t kContextBuffer = 0x10;

} // namespace

GameDriver::GameDriver(const Rom& rom, const DriverInfo& info) : info_(info), system_(rom)
{
    for (uint32_t address = info_.copy_start; address < info_.copy_end; address++)
    {
        system_.Store(address, 1, rom.U8(info_.image_base + (address - kIwramBase)));
    }

    for (const GameCall& call : info_.init)
    {
        system_.Call(call.routine, call.arguments);
    }
}

void GameDriver::Start(uint32_t module, uint32_t order, int32_t step)
{
    system_.Call(info_.start, std::array<uint32_t, 3>{module, order, uint32_t(step)});
}

void GameDriver::Frame()
{
    system_.Call(info_.player);
    system_.Call(info_.mixer, std::array<uint32_t, 1>{info_.context});

    const uint32_t buffer = info_.output + system_.Load(info_.context + kContextBuffer, 4);
    output_.resize(info_.mixer_settings.samples);
    for (uint32_t i = 0; i < info_.mixer_settings.samples; i++)
    {
        output_[i] = int8_t(system_.Load(buffer + i, 1));
    }
}

const std::vector<int8_t>& GameDriver::Output() const
{
    return output_;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace agblib::frontline_mod
