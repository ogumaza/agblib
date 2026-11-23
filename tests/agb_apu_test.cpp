// SPDX-License-Identifier: MIT

// Unit tests for the model of the GBA's sound hardware (agblib/agb/apu.h). The expected values come from GBATEK's
// description of the GBA's sound and Pan Docs' description of the Game Boy's channels. A few come from mGBA 0.11.0,
// where neither says: the wave's lag of one digit and the level that a stopped wave keeps. agb_apu_enhanced_test.cpp
// tests the enhanced mode and the grown PSG units.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/testing/check.h"

#include "agb_apu_test_util.h"

namespace agblib::agb::apu_test
{
namespace
{

using seqlib::testing::Throws;

// Runs `samples` output samples, and returns the left side's.
std::vector<int16_t> Left(Apu& apu, size_t samples)
{
    std::vector<int16_t> out;
    apu.Run(samples * (kSecondCycles / uint64_t(apu.Rate())), out);
    std::vector<int16_t> left;
    for (size_t i = 0; i < out.size(); i += 2)
    {
        left.push_back(out[i]);
    }

    return left;
}

// Pan Docs' waveforms of the squares' duties, from the first step: 12.5%, 25%, 50% and 75%.
void TestSquareDuties()
{
    const char* waveforms[4] = {"00000001", "10000001", "10000111", "01111110"};
    for (int duty = 0; duty < 4; ++duty)
    {
        Apu apu = PoweredOn();
        apu.Store(kRegNr11, 1, uint32_t(duty << 6));
        apu.Store(kRegNr12, 1, 0xF0);
        apu.Store(kRegNr13, 2, 0x8400); // a frequency of 1024 and a restart: 16,384 cycles, 32 samples, a step
        const auto left = Left(apu, 512);
        bool matches = true;
        for (size_t i = 0; i < left.size(); ++i)
        {
            matches &= left[i] == (waveforms[duty][i / 32 % 8] == '1' ? 15 * kLevel : 0);
        }

        XSFLIB_CHECK(matches);
    }
}

// GBATEK: a square plays 131,072 / (2048 - n) Hz. A frequency of 1792 gives 512 Hz: a rise every 64 samples.
void TestSquareFrequency()
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr21, 1, 0x80);
    apu.Store(kRegNr22, 1, 0xF0);
    apu.Store(kRegNr23, 2, 0x8700); // 0x700: 1792
    const auto left = Left(apu, 1024);
    std::vector<size_t> rises;
    for (size_t i = 1; i < left.size(); ++i)
    {
        if (left[i] != 0 && left[i - 1] == 0)
        {
            rises.push_back(i);
        }
    }

    XSFLIB_CHECK(rises.size() == 16);
    bool apart = true;
    for (size_t k = 1; k < rises.size(); ++k)
    {
        apart &= rises[k] - rises[k - 1] == 64;
    }

    XSFLIB_CHECK(apart);
}

// Pan Docs: an envelope steps every n / 64 s, at the frame sequencer's step 7, up to 15 or down to 0. A square at a
// step a sample, 75% high, shows its level at steps 1 to 6 of its duty.
std::vector<int> Levels(uint8_t nr12, size_t samples)
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr11, 1, 0xC0);
    apu.Store(kRegNr12, 1, nr12);
    apu.Store(kRegNr13, 2, 0x87E0); // 0x7E0: 2016, a step of 512 cycles
    apu.SetSequencer(7, 10 * kSampleCycles);
    const auto left = Left(apu, samples);
    std::vector<int> levels;
    for (size_t i = 0; i < left.size(); ++i)
    {
        if (i % 8 >= 1 && i % 8 <= 6)
        {
            levels.push_back(left[i] / kLevel);
        }
        else if (left[i] != 0)
        {
            levels.push_back(-1); // a low step that isn't 0
        }
    }

    return levels;
}

void TestEnvelope()
{
    // The envelope's steps come at sample 10 and every 512 samples after it.
    const auto expected = [](size_t samples, int start, int direction, int period)
    {
        std::vector<int> levels;
        for (size_t i = 0; i < samples; ++i)
        {
            if (i % 8 < 1 || i % 8 > 6)
            {
                continue;
            }

            const int ticks = i < 10 ? 0 : int(1 + (i - 10) / 512);
            const int steps = ticks / period;
            levels.push_back(std::clamp(start + direction * steps, 0, 15));
        }

        return levels;
    };
    XSFLIB_CHECK(Levels(0xF1, 512 * 18) == expected(512 * 18, 15, -1, 1));
    XSFLIB_CHECK(Levels(0x09, 512 * 18) == expected(512 * 18, 0, 1, 1));
    XSFLIB_CHECK(Levels(0xA3, 512 * 12) == expected(512 * 12, 10, -1, 3));
    XSFLIB_CHECK(Levels(0x70, 512 * 4) == expected(512 * 4, 7, 0, 1));
}

// GBATEK: a square plays (64 - t) / 256 s with its length on, the noise as well, and the wave (256 - t) / 256 s. Pan
// Docs: the lengths count at the frame sequencer's steps 0, 2, 4 and 6, and a length of 0 counts 64 or 256.
void TestLength()
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr11, 1, 0x80 | 60); // 4
    apu.Store(kRegNr12, 1, 0xF0);
    apu.Store(kRegNr13, 2, 0xC400); // a restart with the length on
    apu.SetSequencer(0, 1000);
    std::vector<int16_t> out;
    apu.Run(1000 + 3 * 2 * kSequencerCycles - 1, out);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 1) == 1);
    apu.Run(1, out);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 1) == 0);

    // A length of 0 counts 64, and the wave's 256: 32 and 128 steps 0, 2, 4 and 6.
    apu = PoweredOn();
    apu.Store(kRegNr41, 1, 0);
    apu.Store(kRegNr42, 1, 0xF0);
    apu.Store(kRegNr44, 1, 0xC0);
    apu.Store(kRegNr30, 1, 0x80);
    apu.Store(kRegNr31, 1, 0);
    apu.Store(kRegNr34, 1, 0xC0);
    apu.SetSequencer(0, 1);
    apu.Run(1 + 63 * 2 * kSequencerCycles - 1, out);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 0x0C) == 0x0C);
    apu.Run(1, out);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 0x0C) == 0x04);
    apu.Run(192 * 2 * kSequencerCycles - 1, out);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 0x04) == 0x04);
    apu.Run(1, out);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 0x04) == 0);

    // Without its length on, a channel plays on.
    apu = PoweredOn();
    apu.Store(kRegNr11, 1, 0x80 | 63);
    apu.Store(kRegNr12, 1, 0xF0);
    apu.Store(kRegNr13, 2, 0x8400);
    apu.Run(20 * kSequencerCycles, out);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 1) == 1);
}

// GBATEK: the sweep gives X(t) = X(t-1) +/- X(t-1) / 2^n every time units of 7.8 ms. Pan Docs: it steps at the frame
// sequencer's steps 2 and 6, and turns the channel off when the next frequency would pass 2047.
void TestSweep()
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr10, 1, 0x11); // a time of 1, up, a shift of 1
    apu.Store(kRegNr12, 1, 0xF0);
    apu.Store(kRegNr13, 2, 0x8100); // 256
    apu.SetSequencer(2, 100);
    const auto frequency = [&]
    {
        return apu.Register(kRegNr13) | (apu.Register(kRegNr14) & 7) << 8;
    };
    std::vector<int16_t> out;
    apu.Run(99, out);
    XSFLIB_CHECK(frequency() == 256);
    for (int expected : {384, 576, 864, 1296})
    {
        apu.Run(1, out);
        XSFLIB_CHECK(frequency() == expected);
        XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 1) == 1);
        apu.Run(4 * kSequencerCycles - 1, out);
    }
    // 1944 takes its place, and 2916 would pass 2047.
    apu.Run(1, out);
    XSFLIB_CHECK(frequency() == 1944);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 1) == 0);

    // Down, with a time of 2: every other step 2 or 6.
    apu = PoweredOn();
    apu.Store(kRegNr10, 1, 0x29);
    apu.Store(kRegNr12, 1, 0xF0);
    apu.Store(kRegNr13, 2, 0x8100);
    apu.SetSequencer(2, 100);
    apu.Run(100 + 4 * kSequencerCycles, out);
    XSFLIB_CHECK(frequency() == 128);
    apu.Run(8 * kSequencerCycles, out);
    XSFLIB_CHECK(frequency() == 64);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 1) == 1);
}

// The wave's digits 0 to 15, then 15 down to 0, in bank 1. Bank 0 plays while the CPU writes them.
Apu WithWave(uint8_t nr30, uint8_t nr32)
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr30, 1, 0x00);
    const uint8_t bytes[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                               0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
    for (uint32_t i = 0; i < 16; ++i)
    {
        apu.Store(kRegWaveRam + i, 1, bytes[i]);
    }

    apu.Store(kRegNr30, 1, nr30);
    apu.Store(kRegNr32, 1, nr32);
    apu.Store(kRegNr33, 2, 0x87C0); // 0x7C0: 1984, a digit of 512 cycles
    return apu;
}

int Digit(size_t n)
{
    n %= 32;
    return n < 16 ? int(n) : int(31 - n);
}

// GBATEK: the wave plays its 32 digits, the high nibble of each byte first, at 2,097,152 / (2048 - n) digits a second.
// Each digit sounds a step late: the first sample plays the digit before the first (mGBA 0.11.0).
void TestWave()
{
    Apu apu = WithWave(0xC0, 0x20); // bank 1, at 100%
    const auto left = Left(apu, 96);
    bool matches = true;
    for (size_t i = 0; i < left.size(); ++i)
    {
        matches &= left[i] == Digit(i + 31) * kLevel;
    }

    XSFLIB_CHECK(matches);

    // GBATEK's volumes: 0%, 100%, 50%, 25%, and 75% with bit 7. The first sample after digit 15 plays it.
    const int volumes[5][2] = {{0x00, 0}, {0x20, 15}, {0x40, 7}, {0x60, 3}, {0x80, 11}};
    for (const auto& [nr32, level] : volumes)
    {
        Apu loud = WithWave(0xC0, uint8_t(nr32));
        XSFLIB_CHECK(Left(loud, 17)[16] == level * kLevel);
    }

    // 64 digits: bank 0, then bank 1. Bank 0 contains the same digits, once the CPU has written them there.
    apu = WithWave(0xC0, 0x20);
    const uint8_t zeros[16] = {};
    for (uint32_t i = 0; i < 16; ++i)
    {
        apu.Store(kRegWaveRam + i, 1, zeros[i]); // bank 0 while bank 1 plays
    }

    apu.Store(kRegNr30, 1, 0xA0);
    apu.Store(kRegNr34, 1, 0x87);
    const auto both = Left(apu, 66);
    // The position is 0: the first sample plays bank 1's last digit.
    XSFLIB_CHECK(both[0] == 0);
    bool banks = true;
    for (size_t i = 1; i <= 32; ++i)
    {
        banks &= both[i] == 0;
    }

    for (size_t i = 33; i < 65; ++i)
    {
        banks &= both[i] == Digit(i - 33) * kLevel;
    }

    XSFLIB_CHECK(banks);
}

// GBATEK: the wave RAM is a shift register. A restart leaves the position, and the next digit comes a whole step later.
// A stopped wave keeps its last level (mGBA 0.11.0).
void TestWaveRestartAndStop()
{
    Apu apu = WithWave(0xC0, 0x20);
    std::vector<int16_t> out;
    apu.Run(10 * kSampleCycles + 256, out); // position 10, halfway through its step
    apu.Store(kRegNr34, 1, 0x87);
    const auto after = Left(apu, 3);
    // Without the restart, the next step would come before the first of these samples.
    XSFLIB_CHECK(after[0] == Digit(9) * kLevel);
    XSFLIB_CHECK(after[1] == Digit(10) * kLevel);
    XSFLIB_CHECK(after[2] == Digit(11) * kLevel);

    // Halfway through digit 12's step.
    apu.Run(256, out);

    apu.Store(kRegNr30, 1, 0x40);
    const auto held = Left(apu, 40);
    bool holds = true;
    for (int16_t sample : held)
    {
        holds &= sample == Digit(12) * kLevel;
    }

    XSFLIB_CHECK(holds);
    XSFLIB_CHECK((apu.Register(kRegSoundCntX) & 4) == 0);
}

// GBATEK: the noise steps 524,288 Hz / r / 2^(s+1) times a second, r = 0.5 for a ratio of 0. Each step shifts the
// counter right, and a carry takes 0x60 (7 bits) or 0x6000 (15 bits) into it. GBATEK's output is high with a carry: the
// Game Boy's analogue output. The output falls as the level rises. The channel gives its level without a carry. mGBA
// 0.11.0 does the same. GBATEK leaves the output before the counter's first step open. mGBA's recordings stay silent
// until that step.
void TestNoise()
{
    for (const bool seven : {true, false})
    {
        Apu apu = PoweredOn();
        apu.Store(kRegNr42, 1, 0xF0);
        apu.Store(kRegNr43, 1, seven ? 0x39u : 0x31u); // a shift of 3, a ratio of 1: a step of 512 cycles
        apu.Store(kRegNr44, 1, 0x80);
        const auto left = Left(apu, 300);
        uint32_t counter = seven ? 0x40 : 0x4000;
        bool carry = true;
        bool matches = left[0] == 0;
        for (size_t i = 1; i < left.size(); ++i)
        {
            carry = (counter & 1) != 0;
            counter >>= 1;
            if (carry)
            {
                counter ^= seven ? 0x60 : 0x6000;
            }
            matches &= left[i] == (carry ? 0 : 15 * kLevel);
        }

        XSFLIB_CHECK(matches);
        if (seven)
        {
            // 127 steps a period.
            bool repeats = true;
            for (size_t i = 1; i + 127 < left.size(); ++i)
            {
                repeats &= left[i] == left[i + 127];
            }

            XSFLIB_CHECK(repeats);
        }
    }

    // A ratio of 0 counts half: a shift of 4 steps every 512 cycles too.
    Apu a = PoweredOn();
    Apu b = PoweredOn();
    for (Apu* apu : {&a, &b})
    {
        apu->Store(kRegNr42, 1, 0xF0);
    }

    a.Store(kRegNr43, 1, 0x48);
    b.Store(kRegNr43, 1, 0x39);
    a.Store(kRegNr44, 1, 0x80);
    b.Store(kRegNr44, 1, 0x80);
    XSFLIB_CHECK(Left(a, 200) == Left(b, 200));
}

// GBATEK: a timer's overflow takes a sample from each FIFO on it. A FIFO's sample counts 2 or 4 times, at 50% or 100%,
// and an empty FIFO goes on with its last sample.
void TestFifos()
{
    Apu apu = PoweredOn();
    apu.Store(kRegSoundCntH, 2, 0x7304); // A at 100% on timer 0, B at 50% on timer 1, both sides
    const int8_t a[] = {10, 20, 30};
    const int8_t b[] = {-7};
    apu.Queue(0, a);
    apu.Queue(1, b);
    apu.SetTimer(0, 1024, 100);
    apu.SetTimer(1, 5000, 3000);
    XSFLIB_CHECK(apu.Queued(0) == 3);
    std::vector<int16_t> out;
    apu.Run(16 * kSampleCycles, out);
    XSFLIB_CHECK(apu.Queued(0) == 0 && apu.Queued(1) == 0);
    // Timer 0 overflows at cycles 100, 1124 and 2148, timer 1 at 3000. The samples fall every 512 cycles.
    const int expected_a[16] = {0, 10, 10, 20, 20, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30};
    bool matches = true;
    for (size_t i = 0; i < 16; ++i)
    {
        const int sample_b = i * kSampleCycles > 3000 ? -7 : 0;
        const int sum = expected_a[i] * 4 + sample_b * 2;
        matches &= out[2 * i] == sum * 64 && out[2 * i + 1] == sum * 64;
    }

    XSFLIB_CHECK(matches);

    // Bits 11 and 15 empty a FIFO. The FIFO then gives 0.
    apu.Queue(0, a);
    apu.Store(kRegSoundCntH + 1, 1, 0x08 | 0x73);
    XSFLIB_CHECK(apu.Queued(0) == 0);
    XSFLIB_CHECK(apu.Register(kRegSoundCntH + 1) == 0x73);
    out.clear();
    apu.Run(kSampleCycles, out);
    XSFLIB_CHECK(out[0] == -7 * 2 * 64);
}

// GBATEK's output stage: the PSG's levels times the master volume plus 1, times 2, at 25%, 50% or 100%, plus the
// FIFOs, plus the bias, clipped to 0 to 1023, keeping the resolution's top bits.
void TestOutputStage()
{
    const auto first = [](Apu& apu)
    {
        return Left(apu, 1)[0];
    };
    const auto square = [](uint16_t cnt_h, uint8_t nr50)
    {
        Apu apu = PoweredOn();
        apu.Store(kRegSoundCntH, 2, cnt_h);
        apu.Store(kRegNr50, 1, nr50);
        apu.Store(kRegNr11, 1, 0xC0);
        apu.Store(kRegNr12, 1, 0xF0);
        apu.Store(kRegNr13, 2, 0x8000);
        std::vector<int16_t> out;
        apu.Run(2 * 32768, out); // a step of 32,768 cycles: the last sample lies in step 1, a high step
        return out;
    };
    // Level 15 at master volume 7: 240 steps at 100%, 120 at 50% and 60 at 25%. Master volume 3 on the left: 120.
    const int16_t full = square(0x0002, 0x77).back();
    XSFLIB_CHECK(full == 240 * 64);
    XSFLIB_CHECK(square(0x0001, 0x77).back() == 120 * 64);
    XSFLIB_CHECK(square(0x0000, 0x77).back() == 60 * 64);
    const auto sides = square(0x0002, 0x37);
    XSFLIB_CHECK(sides[sides.size() - 2] == 120 * 64 && sides.back() == 240 * 64);

    // Clipping: two FIFOs of 127 at 100% reach 512 + 1016, past 1023. Two of -128 reach -512, below 0.
    for (const auto& [sample, expected] : {std::pair<int8_t, int>{127, (1022 - 512) * 64}, {-128, -512 * 64}})
    {
        Apu apu = PoweredOn();
        apu.Store(kRegSoundCntH, 2, 0x330C);
        const int8_t samples[] = {sample};
        apu.Queue(0, samples);
        apu.Queue(1, samples);
        apu.SetTimer(0, 100, 0);
        XSFLIB_CHECK(first(apu) == expected);
    }

    // The resolution: a FIFO's 1 at 50% adds 2 steps. At 32,768 Hz the output keeps 9 bits, 512 + 2. At 65,536 Hz it
    // keeps 8 and drops the 2.
    for (const auto& [bias, expected] : {std::pair<uint16_t, int>{0x0200, 2 * 64}, {0x4200, 0}})
    {
        Apu apu = PoweredOn();
        apu.Store(kRegSoundBias, 2, bias);
        apu.Store(kRegSoundCntH, 2, 0x0300);
        const int8_t samples[] = {1};
        apu.Queue(0, samples);
        apu.SetTimer(0, 100, 0);
        XSFLIB_CHECK(first(apu) == expected);
    }

    // A side's enables: NR51's channels and SOUNDCNT_H's FIFOs.
    Apu apu = PoweredOn();
    apu.Store(kRegNr51, 1, 0x10);        // square 1 on the left only
    apu.Store(kRegSoundCntH, 2, 0x0106); // FIFO A at 100% on the right only
    apu.Store(kRegNr11, 1, 0xC0);
    apu.Store(kRegNr12, 1, 0xF0);
    apu.Store(kRegNr13, 2, 0x8000);
    const int8_t samples[] = {-50};
    apu.Queue(0, samples);
    apu.SetTimer(0, 100, 0);
    std::vector<int16_t> out;
    apu.Run(2 * 32768 + kSampleCycles, out); // step 1 of the square
    XSFLIB_CHECK(out[out.size() - 2] == 240 * 64);
    XSFLIB_CHECK(out.back() == -200 * 64);

    // The bias sets the middle: without the sound on, the output is 0.
    Apu off;
    XSFLIB_CHECK(first(off) == 0);
}

// GBATEK: the rates of the resolutions 9 to 6 bits: 32,768, 65,536, 131,072 and 262,144 Hz. The model puts out the
// higher two at 65,536 Hz.
void TestRate()
{
    const int rates[4] = {32768, 65536, 65536, 65536};
    for (uint32_t resolution = 0; resolution < 4; ++resolution)
    {
        Apu apu;
        apu.Store(kRegSoundBias, 2, 0x200 | resolution << 14);
        XSFLIB_CHECK(apu.Rate() == rates[resolution]);
        std::vector<int16_t> out;
        apu.Run(kSecondCycles / 64, out);
        XSFLIB_CHECK(out.size() == size_t(2 * rates[resolution] / 64));
    }
}

// Pan Docs: turning the sound off clears NR10 to NR51 and ignores their stores until it's on again. The wave RAM keeps
// its digits. SOUNDCNT_X's bits 0 to 3 give the channels that play.
void TestPower()
{
    Apu apu = WithWave(0xC0, 0x20);
    apu.Store(kRegNr12, 1, 0xF0);
    apu.Store(kRegNr13, 2, 0x8400);
    XSFLIB_CHECK(apu.Register(kRegSoundCntX) == 0x85);
    apu.Store(kRegSoundCntX, 1, 0x00);
    XSFLIB_CHECK(apu.Register(kRegSoundCntX) == 0x00);
    bool cleared = true;
    for (uint32_t address = kRegNr10; address <= kRegNr51; ++address)
    {
        cleared &= apu.Register(address) == 0;
    }

    XSFLIB_CHECK(cleared);
    apu.Store(kRegNr50, 1, 0x77);
    XSFLIB_CHECK(apu.Register(kRegNr50) == 0);
    XSFLIB_CHECK(apu.Register(kRegSoundBias + 1) == 0x02);
    std::vector<int16_t> out;
    apu.Run(kSampleCycles, out);
    XSFLIB_CHECK(out[0] == 0 && out[1] == 0);

    // On again: the CPU reaches bank 1 with the digits while bank 0 plays.
    apu.Store(kRegSoundCntX, 1, 0x80);
    XSFLIB_CHECK(apu.Register(kRegWaveRam) == 0x01 && apu.Register(kRegWaveRam + 15) == 0x10);
}

// The registers as a game's init leaves them: every channel off, and the FIFOs' resets read back as 0.
void TestRegisters()
{
    std::vector<uint8_t> registers(kSoundRegisterBytes);
    registers[kRegNr50 - kRegSound1CntL] = 0x77;
    registers[kRegSoundCntH - kRegSound1CntL + 1] = 0x8B;
    registers[kRegSoundCntX - kRegSound1CntL] = 0x8F;
    registers[kRegSoundBias - kRegSound1CntL + 1] = 0x42;
    registers[kRegWaveRam - kRegSound1CntL] = 0x5A;
    const Apu apu(registers);
    XSFLIB_CHECK(apu.Register(kRegNr50) == 0x77);
    XSFLIB_CHECK(apu.Register(kRegSoundCntH + 1) == 0x03);
    XSFLIB_CHECK(apu.Register(kRegSoundCntX) == 0x80);
    XSFLIB_CHECK(apu.Rate() == 65536);
    XSFLIB_CHECK(apu.Register(kRegWaveRam) == 0x5A);

    const bool refused = Throws<std::invalid_argument>([&] { static_cast<void>(Apu(std::vector<uint8_t>(10))); });

    XSFLIB_CHECK(refused);
}

// Muted channels leave the output, and their state goes on.
void TestMutes()
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr11, 1, 0xC0);
    apu.Store(kRegNr12, 1, 0xF0);
    apu.Store(kRegNr13, 2, 0x8000);
    apu.Store(kRegSoundCntH, 2, 0x0306);
    const int8_t samples[] = {25};
    apu.Queue(0, samples);
    apu.SetTimer(0, 100, 0);
    apu.SetMuted(kApuSquare1 | kApuSquare2);
    std::vector<int16_t> out;
    apu.Run(2 * 32768 + kSampleCycles, out);
    XSFLIB_CHECK(out.back() == 100 * 64);
    apu.SetMuted(kApuFifoA);
    out.clear();
    apu.Run(kSampleCycles, out);
    XSFLIB_CHECK(out.back() == 240 * 64);
}

void TestSequencerRange()
{
    Apu apu;
    for (const auto& [step, cycles] : {std::pair<int, uint32_t>{8, 1}, {-1, 1}, {0, 0}, {0, kSequencerCycles + 1}})
    {
        XSFLIB_CHECK(Throws<std::invalid_argument>([&] { apu.SetSequencer(step, cycles); }));
    }
}

int RunTests()
{
    return xsflib::testing::RunTest("agb_apu",
                                    []
                                    {
                                        TestSquareDuties();
                                        TestSquareFrequency();
                                        TestEnvelope();
                                        TestLength();
                                        TestSweep();
                                        TestWave();
                                        TestWaveRestartAndStop();
                                        TestNoise();
                                        TestFifos();
                                        TestOutputStage();
                                        TestRate();
                                        TestPower();
                                        TestRegisters();
                                        TestMutes();
                                        TestSequencerRange();
                                    });
}

} // namespace
} // namespace agblib::agb::apu_test

int main()
{
    return agblib::agb::apu_test::RunTests();
}
