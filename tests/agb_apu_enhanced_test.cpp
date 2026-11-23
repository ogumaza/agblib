// SPDX-License-Identifier: MIT

// Unit tests for the enhanced mode and the grown PSG units of the model of the GBA's sound hardware
// (agblib/agb/apu.h). The enhanced mode's values come from the header's rules: the output stage's volumes without the
// shift's rounding, the band-limited steps' response and delay, and a square wave's Fourier series. A grown PSG unit
// has to play as the console's unit plays with the same stores: the header's rule.

#include <algorithm>
#include <array>
#include <cmath>
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

// The enhanced mode's samples of `cycles`, left then right, split into the two sides.
std::pair<std::vector<float>, std::vector<float>> Enhanced(Apu& apu, uint64_t cycles)
{
    std::vector<float> out;
    apu.Run(cycles, out);
    std::pair<std::vector<float>, std::vector<float>> sides;
    for (size_t i = 0; i < out.size(); i += 2)
    {
        sides.first.push_back(out[i]);
        sides.second.push_back(out[i + 1]);
    }

    return sides;
}

// Each frame of kFrameCycles cycles gives kEnhancedFrameSamples samples: those whose times are before the run's end.
void TestEnhancedSamples()
{
    Apu apu = PoweredOn();
    apu.StartEnhanced();
    XSFLIB_CHECK(apu.Enhanced());
    XSFLIB_CHECK(Enhanced(apu, kFrameCycles).first.size() == kEnhancedFrameSamples);
    // 1,000 cycles contain the times of samples 804 to 806: 804 + 1000 * 804 / 280896 is 806.9.
    XSFLIB_CHECK(Enhanced(apu, 1000).first.size() == 3);
    XSFLIB_CHECK(Enhanced(apu, kFrameCycles - 1000).first.size() == kEnhancedFrameSamples - 3);

    std::vector<int16_t> strict;
    XSFLIB_CHECK(Throws<std::logic_error>([&] { apu.Run(100, strict); }));

    Apu console = PoweredOn();
    std::vector<float> enhanced;
    const float samples[] = {1.0f};
    XSFLIB_CHECK(Throws<std::logic_error>([&] { console.Run(100, enhanced); }));
    XSFLIB_CHECK(Throws<std::logic_error>([&] { console.QueueEnhanced(0, samples); }));
}

// A slow square's plateaus are the levels exactly, at 128 * (master volume + 1) >> share steps of a 16-bit sample for
// each step of the level. The console's mode rounds the sum down after the shift: with master volume 2 at 25%, it gives
// 22 * 64 for level 15, and the enhanced mode 15 * 96.
void TestEnhancedVolumes()
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr50, 1, 0x25); // left 2, right 5
    apu.Store(kRegNr51, 1, 0x11);
    apu.Store(kRegSoundCntH, 2, 0x0000); // the PSG at 25%
    apu.StartEnhanced();
    apu.Store(kRegNr11, 1, 0x80);   // 50%
    apu.Store(kRegNr12, 1, 0xF0);   // level 15
    apu.Store(kRegNr13, 2, 0x8000); // frequency 0: 32,768 cycles a step
    const auto [left, right] = Enhanced(apu, 8 * 32768);

    // Steps 0, 5, 6 and 7 are high. The fall comes at cycle 32,768, sample 93.8, and the rise at cycle 163,840, sample
    // 469.0. A change reaches from 32 samples before its time to 32 after it, and the output runs 32 samples late. Away
    // from the changes, the plateaus are the levels exactly.
    const float high_left = 15.0f * 96.0f / 32768.0f;
    const float high_right = 15.0f * 192.0f / 32768.0f;
    bool plateaus = true;
    for (size_t i = 0; i < left.size(); ++i)
    {
        if (i <= 92)
        {
            plateaus &= left[i] == high_left && right[i] == high_right;
        }
        else if (i >= 159 && i <= 468)
        {
            plateaus &= left[i] == 0.0f && right[i] == 0.0f;
        }
        else if (i >= 535)
        {
            plateaus &= left[i] == high_left && right[i] == high_right;
        }
    }

    XSFLIB_CHECK(plateaus);
    XSFLIB_CHECK(left[125] > 0.5f * high_left && left[126] < 0.5f * high_left); // the fall at output sample 125.8

    // A mute takes the square out at once, and its end brings it back. The mute comes at cycle 262,144, sample 750.0,
    // in a high step. The square is high again from cycle 688,128 to 819,200: samples 1,969.6 to 2,344.8.
    apu.SetMuted(kApuSquare1);
    const auto muted = Enhanced(apu, kFrameCycles);
    XSFLIB_CHECK(muted.first[100] == 0.0f && muted.second[100] == 0.0f);
    apu.SetMuted(0);
    const auto unmuted = Enhanced(apu, kFrameCycles);
    XSFLIB_CHECK(unmuted.first[2200 - 1554] == high_left && unmuted.second[2200 - 1554] == high_right);
}

// An envelope's step changes the level at the frame sequencer's step, between the timer's steps. The frame sequencer's
// step 7 comes at cycle 262,144, sample 750.3, while the square is high from cycle 160,000 to 288,000: samples 458.0 to
// 824.3. Level 15 lasts until 33 samples after the high step's start plus the output's delay of 32. Level 14 lasts from
// 33 samples after the envelope's step until 33 before the square falls.
void TestEnhancedEnvelope()
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr50, 1, 0x77);
    apu.Store(kRegNr51, 1, 0x11);
    apu.Store(kRegSoundCntH, 2, 0x0002); // the PSG at 100%
    apu.StartEnhanced();
    apu.Store(kRegNr11, 1, 0x80);        // 50%
    apu.Store(kRegNr12, 1, 0xF1);        // level 15, down by 1 at each of the envelope's steps
    apu.Store(kRegNr13, 2, 0x8000 | 48); // 32,000 cycles a step
    const std::vector<float> left = Enhanced(apu, 2 * kFrameCycles).first;
    bool fifteen = true;
    for (size_t i = 523; i <= 749; ++i)
    {
        fifteen &= left[i] == 15.0f * 1024.0f / 32768.0f;
    }

    bool fourteen = true;
    for (size_t i = 816; i <= 823; ++i)
    {
        fourteen &= left[i] == 14.0f * 1024.0f / 32768.0f;
    }

    XSFLIB_CHECK(fifteen && fourteen);
}

// A square of 50% at 789.6 Hz: its harmonics below 0.84 of the Nyquist frequency keep their amplitudes within 0.01 dB.
// The square spans 15 * 1,024 steps of a 16-bit sample: 0.46875 of full scale.
void TestEnhancedSquare()
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr51, 1, 0x11);
    apu.StartEnhanced();
    apu.Store(kRegNr11, 1, 0x80);
    apu.Store(kRegNr12, 1, 0xF0);
    apu.Store(kRegNr13, 2, 0x8000 | 1882); // 131,072 / 166 Hz
    const std::vector<float> left = Enhanced(apu, 100 * kFrameCycles).first;
    const std::vector<float> signal(left.begin() + 804, left.begin() + 804 + 65536);
    constexpr double kPi = 3.14159265358979323846;
    const double rate = double(kSecondCycles) * kEnhancedFrameSamples / kFrameCycles;
    const double fundamental = 131072.0 / 166.0;
    double worst = 0.0;
    for (int k = 1; k * fundamental <= 0.84 * rate / 2.0; k += 2)
    {
        double re = 0.0;
        double im = 0.0;
        double gain = 0.0;
        const double n = double(signal.size());
        for (size_t i = 0; i < signal.size(); ++i)
        {
            const double t = 2.0 * kPi * double(i) / n;
            const double window =
                0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
            const double angle = 2.0 * kPi * k * fundamental * double(i) / rate;
            re += window * signal[i] * std::cos(angle);
            im += window * signal[i] * std::sin(angle);
            gain += window;
        }

        const double amplitude = 2.0 * std::hypot(re, im) / gain;
        const double expected = 2.0 * 0.46875 / (k * kPi);
        worst = std::max(worst, std::fabs(20.0 * std::log10(amplitude / expected)));
    }

    XSFLIB_CHECK(worst <= 0.01);
}

// The FIFOs' samples count 2 or 4 times on the sides that SOUNDCNT_H gives them. They start after the band-limited
// steps' delay of 32 samples. A run without output drops the samples of its time, and the PSG then goes on from its
// level.
void TestEnhancedFifos()
{
    Apu apu = PoweredOn();
    apu.Store(kRegNr51, 1, 0x00);
    apu.Store(kRegSoundCntH, 2, 0x6106); // A at 100% on the right, B at 50% on the left
    apu.StartEnhanced();
    std::vector<float> ramp(3000);
    for (size_t i = 0; i < ramp.size(); ++i)
    {
        ramp[i] = float(i % 100) - 50.0f;
    }

    const float b[] = {3.0f, -1.0f};
    apu.QueueEnhanced(0, ramp);
    apu.QueueEnhanced(1, b);
    const auto [left, right] = Enhanced(apu, kFrameCycles);
    bool matches = left[31] == 0.0f && right[31] == 0.0f;
    matches &= left[32] * 32768.0f == 3.0f * 128.0f && left[33] * 32768.0f == -128.0f && left[34] == 0.0f;
    for (size_t i = 32; i < left.size(); ++i)
    {
        matches &= right[i] * 32768.0f == ramp[i - 32] * 256.0f;
    }

    XSFLIB_CHECK(matches);

    // A frame without output takes the ramp's next 804 samples.
    apu.Run(kFrameCycles);
    const auto after = Enhanced(apu, kFrameCycles);
    XSFLIB_CHECK(after.second[0] * 32768.0f == ramp[2 * 804 - 32] * 256.0f);
}

// The console's register at `address` in grown PSG unit `unit`.
uint32_t InUnit(int unit, uint32_t address)
{
    return kGrownPsgRegisters + uint32_t(unit - 1) * kGrownPsgBytes + (address - kRegSound1CntL);
}

// Stores a square 1 that sweeps up and a wave channel with its pattern in unit `unit`, and sends both to the sides in
// the unit's NR51. The console's wave RAM takes another pattern.
void StoreGrownNotes(Apu& apu, int unit)
{
    const auto at = [unit](uint32_t address)
    {
        return unit == 0 ? address : InUnit(unit, address);
    };
    for (uint32_t i = 0; i < 16; i += 4)
    {
        apu.Store(kRegWaveRam + i, 4, 0x01234567u + i);
    }

    apu.Store(at(kRegNr30), 1, 0x40);
    for (uint32_t i = 0; i < 16; i += 4)
    {
        apu.Store(at(kRegWaveRam + i), 4, 0xFEDC9876u - 0x00110011u * i);
    }

    apu.Store(at(kRegNr30), 1, 0x80);
    apu.Store(at(kRegNr32), 1, 0x20);
    apu.Store(at(kRegNr33), 2, 0x8000 | 1800);
    apu.Store(at(kRegNr10), 1, 0x17);
    apu.Store(at(kRegNr11), 1, 0x80);
    apu.Store(at(kRegNr12), 1, 0xF1);
    apu.Store(at(kRegNr13), 2, 0x8000 | 1500);
    apu.Store(at(kRegNr51), 1, 0x15);
}

// A grown unit's channels play as the console's do, with their sweep, envelope and wave RAM. The unit's NR51 sends them
// to the sides. SetMutedUnits mutes the unit alone, and UnitPeaks gives the unit's peaks apart from the console's.
void TestGrownUnits()
{
    for (const bool enhanced : {false, true})
    {
        Apu console = PoweredOn();
        Apu grown = PoweredOn();
        console.MeterChannels(true);
        grown.MeterChannels(true);
        if (enhanced)
        {
            console.StartEnhanced();
            grown.StartEnhanced();
        }

        console.Store(kRegNr51, 1, 0x00);
        grown.Store(kRegNr51, 1, 0x00);
        StoreGrownNotes(console, 0);
        StoreGrownNotes(grown, 1);
        XSFLIB_CHECK(console.PsgUnits() == 1);
        XSFLIB_CHECK(grown.PsgUnits() == 2);
        XSFLIB_CHECK(grown.Register(InUnit(1, kRegNr51)) == 0x15);
        XSFLIB_CHECK(grown.Register(InUnit(1, kRegSoundCntX)) == 0x05);
        XSFLIB_CHECK(grown.Register(kRegSoundCntX) == 0x80);

        if (enhanced)
        {
            const auto a = Enhanced(console, 30 * kFrameCycles);
            const auto b = Enhanced(grown, 30 * kFrameCycles);
            XSFLIB_CHECK(a == b);
            XSFLIB_CHECK(*std::max_element(a.first.begin(), a.first.end()) > 0.1f);
        }
        else
        {
            std::vector<int16_t> a;
            std::vector<int16_t> b;
            console.Run(30 * kFrameCycles, a);
            grown.Run(30 * kFrameCycles, b);
            XSFLIB_CHECK(a == b);
            XSFLIB_CHECK(*std::max_element(a.begin(), a.end()) > 0);
        }

        for (int c = 0; c < 4; ++c)
        {
            XSFLIB_CHECK(grown.UnitPeaks(1, c) == console.ChannelPeaks(c));
            XSFLIB_CHECK((grown.ChannelPeaks(c) == std::array<int32_t, 2>{}));
        }

        // A mute of the unit's square 1 and wave leaves the output silent.
        const uint8_t mutes[] = {uint8_t(kApuSquare1 | 0x04)};
        grown.SetMutedUnits(mutes);
        if (enhanced)
        {
            const auto quiet = Enhanced(grown, 10 * kFrameCycles);
            XSFLIB_CHECK(std::all_of(quiet.first.begin() + 200, quiet.first.end(), [](float x) { return x == 0.0f; }));
        }
        else
        {
            std::vector<int16_t> quiet;
            grown.Run(kFrameCycles, quiet);
            XSFLIB_CHECK(std::all_of(quiet.begin(), quiet.end(), [](int16_t x) { return x == 0; }));
        }
    }
}

// PsgSilent(): a channel that's off makes no sound. Nor does one at a level of 0, unless its envelope rises from there.
// A wave channel keeps its volume in NR32.
void TestPsgSilent()
{
    Apu off = PoweredOn();
    XSFLIB_CHECK(off.PsgSilent());

    // A note at a level of 15, without an envelope.
    Apu square = PoweredOn();
    square.Store(kRegNr11, 1, 0x80);
    square.Store(kRegNr12, 1, 0xF0);
    square.Store(kRegNr13, 2, 0x8400);
    XSFLIB_CHECK(!square.PsgSilent());

    // At a period of 1, the envelope falls by 1 every 8 of the frame sequencer's steps: 15 times from a level of 15.
    Apu falling = PoweredOn();
    falling.Store(kRegNr21, 1, 0x80);
    falling.Store(kRegNr22, 1, 0xF1);
    falling.Store(kRegNr23, 2, 0x8400);
    XSFLIB_CHECK(!falling.PsgSilent());
    falling.Run(14 * 8 * kSequencerCycles);
    XSFLIB_CHECK(!falling.PsgSilent());
    falling.Run(2 * 8 * kSequencerCycles);
    XSFLIB_CHECK(falling.PsgSilent());

    // From a level of 0, an envelope with a period rises. Without a period, it stays at 0. NR42's bit 3 keeps the
    // channel on at that level.
    Apu rising = PoweredOn();
    rising.Store(kRegNr42, 1, 0x09);
    rising.Store(kRegNr43, 2, 0x8000);
    XSFLIB_CHECK(!rising.PsgSilent());
    Apu stopped = PoweredOn();
    stopped.Store(kRegNr42, 1, 0x08);
    stopped.Store(kRegNr43, 2, 0x8000);
    XSFLIB_CHECK(stopped.PsgSilent());

    // A wave channel plays at the volume of NR32's high bits, and a volume of 0 keeps it at a constant.
    Apu wave = PoweredOn();
    wave.Store(kRegNr30, 1, 0x80);
    wave.Store(kRegNr32, 1, 0x20);
    wave.Store(kRegNr33, 2, 0x8400);
    XSFLIB_CHECK(!wave.PsgSilent());
    wave.Store(kRegNr32, 1, 0x00);
    XSFLIB_CHECK(wave.PsgSilent());
    wave.Store(kRegNr32, 1, 0x80);
    XSFLIB_CHECK(!wave.PsgSilent());
    wave.Store(kRegNr30, 1, 0x00);
    XSFLIB_CHECK(wave.PsgSilent());

    // A grown unit's channels count too.
    Apu grown = PoweredOn();
    StoreGrownNotes(grown, 1);
    XSFLIB_CHECK(grown.PsgUnits() == 2);
    XSFLIB_CHECK(!grown.PsgSilent());
}

int RunTests()
{
    return xsflib::testing::RunTest("agb_apu_enhanced",
                                    []
                                    {
                                        TestPsgSilent();
                                        TestGrownUnits();
                                        TestEnhancedSamples();
                                        TestEnhancedVolumes();
                                        TestEnhancedEnvelope();
                                        TestEnhancedSquare();
                                        TestEnhancedFifos();
                                    });
}

} // namespace
} // namespace agblib::agb::apu_test

int main()
{
    return agblib::agb::apu_test::RunTests();
}
