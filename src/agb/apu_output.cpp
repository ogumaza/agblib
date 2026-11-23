// SPDX-License-Identifier: MIT

// The run of the model of the GBA's sound hardware: the PSG's timers, the channels' levels, the output stage, and the
// loop over the cycles that takes the samples of both modes.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"

#include "agblib/agb/apu.h"
#include "apu_internal.h"

namespace agblib::agb
{
namespace
{

// Moves a timer of `period` cycles on by `cycles`. Returns the steps it took.
uint32_t Count(uint32_t& countdown, uint32_t period, uint32_t cycles)
{
    if (cycles < countdown)
    {
        countdown -= cycles;
        return 0;
    }

    cycles -= countdown;
    countdown = period - cycles % period;

    return 1 + cycles / period;
}

} // namespace

void Apu::Advance(Psg& psg, uint32_t cycles)
{
    for (Square& square : psg.squares)
    {
        if (square.on)
        {
            const uint32_t steps = Count(square.countdown, SquarePeriod(square.frequency), cycles);
            square.position = uint8_t((square.position + steps) & 7);
        }
    }

    if (psg.wave.on)
    {
        const uint32_t digits = (psg.registers[kNr30] & 0x20) ? 64 : 32;
        const uint32_t steps = Count(psg.wave.countdown, WavePeriod(psg.wave.frequency), cycles);
        psg.wave.position = uint8_t((psg.wave.position + steps) % digits);
    }

    const uint8_t nr43 = psg.registers[kNr43];
    if (psg.noise.on && (nr43 >> 4) < kNoiseShifts)
    {
        const uint32_t steps = Count(psg.noise.countdown, NoisePeriod(nr43), cycles);
        const uint16_t taps = (nr43 & 8) ? 0x60 : 0x6000;
        for (uint32_t k = 0; k < steps; ++k)
        {
            psg.noise.carry = (psg.noise.lfsr & 1) != 0;
            psg.noise.lfsr = uint16_t(psg.noise.lfsr >> 1);
            if (psg.noise.carry)
            {
                psg.noise.lfsr ^= taps;
            }
        }
    }
}

bool Apu::PsgSilent() const
{
    const auto silent = [](bool on, const Envelope& envelope)
    {
        return !on || (envelope.level == 0 && !(envelope.up && envelope.period != 0));
    };

    for (const Psg& psg : psgs_)
    {
        constexpr uint8_t kWaveVolumeBits = 0xE0;
        if (!silent(psg.squares[0].on, psg.squares[0].envelope) ||
            !silent(psg.squares[1].on, psg.squares[1].envelope) || !silent(psg.noise.on, psg.noise.envelope) ||
            (psg.wave.on && (psg.registers[kNr32] & kWaveVolumeBits) != 0))
        {
            return false;
        }
    }

    return true;
}

int Apu::Level(const Psg& psg, int channel)
{
    switch (channel)
    {
    case 0:
    case 1:
        {
            const Square& square = psg.squares[size_t(channel)];
            return square.on && (kDuties[square.duty] >> (7 - square.position) & 1) ? square.envelope.level : 0;
        }
    case 2:
        {
            if (!psg.wave.on)
            {
                return psg.wave.held;
            }

            const uint8_t nr30 = psg.registers[kNr30];
            const uint32_t digits = (nr30 & 0x20) ? 64 : 32;
            const uint32_t played = (psg.wave.position + digits - 1) % digits;
            const size_t bank = ((nr30 >> 6) ^ (played >> 5)) & 1;
            const uint8_t byte = psg.wave.banks[bank][(played & 31) >> 1];
            const int digit = (played & 1) ? byte & 15 : byte >> 4;
            const uint8_t nr32 = psg.registers[kNr32];
            if (nr32 & 0x80)
            {
                return digit * 3 >> 2;
            }

            constexpr int kShifts[4] = {4, 0, 1, 2};
            return digit >> kShifts[(nr32 >> 5) & 3];
        }
    default:
        return psg.noise.on && !psg.noise.carry ? psg.noise.envelope.level : 0;
    }
}

int16_t Apu::Output(int side)
{
    const std::array<uint8_t, kSoundRegisterBytes>& registers = Registers();
    const int bias = xsflib::LoadLe16(&registers[kSoundBias]) & 0x3FE;
    if ((registers[kSoundCntX] & kMasterEnable) == 0)
    {
        return 0;
    }

    // Side 0 is the left. The left takes the high bits of NR50 and NR51 and SOUNDCNT_H's odd enables. A unit's NR51
    // sends its channels to the sides.
    const int master = (side == 0 ? registers[kNr50] >> 4 : registers[kNr50]) & 7;
    const uint16_t control = xsflib::LoadLe16(&registers[kSoundCntH]);
    constexpr int kShares[4] = {2, 1, 0, 0}; // 25%, 50%, 100%, and 100% for the prohibited setting
    int levels = 0;
    for (size_t unit = 0; unit < psgs_.size(); ++unit)
    {
        const uint8_t nr51 = psgs_[unit].registers[kNr51];
        const uint8_t enables = uint8_t(side == 0 ? nr51 >> 4 : nr51 & 15);
        const uint8_t muted = UnitMuted(unit);
        for (int c = 0; c < 4; ++c)
        {
            if ((enables >> c & 1) && !(muted >> c & 1))
            {
                const int level = Level(psgs_[unit], c);
                levels += level;
                if (metering_)
                {
                    int32_t& peak = Peak(unit, c, size_t(side));
                    peak = std::max(peak, ((level * 2 * (master + 1)) >> kShares[control & 3]) * 64);
                }
            }
        }
    }

    int sum = (levels * 2 * (master + 1)) >> kShares[control & 3];
    for (int f = 0; f < 2; ++f)
    {
        const int enable = 8 + 4 * f + (side == 0 ? 1 : 0);
        if ((control >> enable & 1) && !(muted_ >> (4 + f) & 1))
        {
            const int part = fifos_[size_t(f)].output * ((control >> (2 + f) & 1) ? 4 : 2);
            sum += part;
            if (metering_)
            {
                int32_t& peak = peaks_[size_t(4 + f)][size_t(side)];
                peak = std::max(peak, std::abs(part) * 64);
            }
        }
    }

    const int resolution = registers[kSoundBias + 1] >> 6;
    const int value = std::clamp(bias + sum, 0, 0x3FF) & ~((2 << resolution) - 1);
    return int16_t(std::clamp((value - bias) * 64, -32768, 32767));
}

void Apu::Run(uint64_t cycles, std::vector<int16_t>& out)
{
    if (enhanced_)
    {
        throw std::logic_error("the model is in the enhanced mode");
    }

    RunCycles(cycles, &out, nullptr);
}

void Apu::Run(uint64_t cycles, std::vector<float>& out)
{
    if (!enhanced_)
    {
        throw std::logic_error("the model isn't in the enhanced mode");
    }

    RunCycles(cycles, nullptr, &out);
}

void Apu::Run(uint64_t cycles)
{
    RunCycles(cycles, nullptr, nullptr);
}

void Apu::RunCycles(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    // In the enhanced mode, a run with output after one without starts the steps again at the channels' levels. A run
    // without output leaves them behind.
    const bool stepping = enhanced_ && enhanced_out;
    if (stepping && enhanced_->stale)
    {
        SetEnhancedGains();
        for (size_t side = 0; side < 2; ++side)
        {
            int64_t target = 0;
            for (size_t unit = 0; unit < psgs_.size(); ++unit)
            {
                for (int c = 0; c < 4; ++c)
                {
                    enhanced_->levels[unit][size_t(c)] = Level(psgs_[unit], c);
                    target += enhanced_->gains[unit][side][size_t(c)] * enhanced_->levels[unit][size_t(c)];
                }
            }

            enhanced_->steps[side].Restart(enhanced_->samples, double(target));
        }

        enhanced_->stale = false;
    }

    if (enhanced_ && !enhanced_out)
    {
        enhanced_->stale = true;
    }

    if (stepping && metering_)
    {
        for (size_t side = 0; side < 2; ++side)
        {
            for (size_t unit = 0; unit < psgs_.size(); ++unit)
            {
                for (int c = 0; c < 4; ++c)
                {
                    int32_t& peak = Peak(unit, c, side);
                    peak = std::max(peak, enhanced_->gains[unit][side][size_t(c)] * enhanced_->levels[unit][size_t(c)]);
                }
            }
        }
    }

    while (true)
    {
        // The events of the current cycle: the timers' overflows and the frame sequencer, then the stores that come
        // between runs, then the output. The output of the cycle where a run ends comes in the next run.
        OverflowTimers();
        if (sequencer_ == 0)
        {
            StepSequencer();
            sequencer_ = kSequencerCycles;
            SettleEnhanced();
        }

        if (cycles == 0)
        {
            break;
        }

        if (until_output_ == 0)
        {
            if (out)
            {
                out->push_back(Output(0));
                out->push_back(Output(1));
            }

            until_output_ = uint32_t(kSecondCycles / uint64_t(Rate()));
        }

        uint64_t step = std::min<uint64_t>({cycles, sequencer_, until_output_});
        for (Timer& timer : timers_)
        {
            if (timer.period != 0)
            {
                step = std::min<uint64_t>(step, timer.countdown);
            }
        }

        const uint32_t dt = uint32_t(step);
        for (size_t unit = 0; unit < psgs_.size(); ++unit)
        {
            if (stepping)
            {
                AdvanceEnhanced(unit, dt);
            }
            else
            {
                Advance(psgs_[unit], dt);
            }
        }

        for (Timer& timer : timers_)
        {
            if (timer.period != 0)
            {
                timer.countdown -= dt;
            }
        }

        sequencer_ -= dt;
        until_output_ -= dt;
        cycle_ += dt;
        cycles -= dt;
    }

    if (enhanced_)
    {
        TakeEnhancedSamples(enhanced_out);
    }
}

} // namespace agblib::agb
