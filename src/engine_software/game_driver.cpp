// SPDX-License-Identifier: MIT

#include "agblib/engine_software/game_driver.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/engine_software/driver.h"

namespace agblib::engine_software
{
namespace
{

// The mixer's state: the buffer that the mix fills next.
constexpr uint32_t kMixerOutput = 0x04;

} // namespace

void RunSteps(agb::System& system, std::span<const GameStep> steps)
{
    for (const GameStep& step : steps)
    {
        if (step.size == 0)
        {
            system.Call(step.address, step.values);
        }
        else
        {
            system.Store(step.address, step.size, step.values.at(0));
        }
    }
}

void SetUpManager(agb::System& system, const DriverInfo& info)
{
    RunSteps(system, SetUpSteps(info));
}

GameDriver::GameDriver(const Rom& rom, const DriverInfo& info) : info_(info), system_(rom)
{
    SetUpManager(system_, info_);
}

void GameDriver::Start(int song)
{
    RunSteps(system_, StartSteps(info_, song));
}

void GameDriver::Frame()
{
    const Build& b = info_.build;
    const uint32_t buffer = system_.Load(b.mixer_state + kMixerOutput, 4);
    system_.Call(b.manager.vcount | 1, std::array<uint32_t, 1>{b.manager.object});
    const uint32_t samples = info_.FrameSamples();
    output_.resize(samples);
    for (uint32_t i = 0; i < samples; i++)
    {
        output_[i] = int8_t(system_.Load(buffer + i, 1));
    }

    system_.Call(b.manager.vblank | 1, std::array<uint32_t, 1>{b.manager.object});
}

const std::vector<int8_t>& GameDriver::Output() const
{
    return output_;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace agblib::engine_software
