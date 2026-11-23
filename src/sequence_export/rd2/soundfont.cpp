// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/rd2/soundfont.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/sequence_export/agb/psg.h"
#include "agblib/sequence_export/agb/psg_cycles.h"
#include "agblib/sequence_export/agb/sf2.h"

namespace agblib::sequence_export::rd2
{
namespace
{

using agblib::agb::kFrameRate;
using agblib::sequence_export::agb::AddLoopGuard;
using agblib::sequence_export::agb::AddressName;
using agblib::sequence_export::agb::AppendNoise;
using agblib::sequence_export::agb::CenteredSquareCycle;
using agblib::sequence_export::agb::kLoopGuard;
using agblib::sequence_export::agb::kMiddleC;
using agblib::sequence_export::agb::kMinLoop;
using agblib::sequence_export::agb::LengthenLoop;
using agblib::sequence_export::agb::MakeInstrument;
using agblib::sequence_export::agb::MakePreset;
using agblib::sequence_export::agb::SetCycle;
using agblib::sequence_export::agb::SquareName;
using agblib::sequence_export::agb::Timecents;
using agblib::sequence_export::agb::WavePatternCycle;

// The longest sample read, in points.
constexpr uint32_t kMaxSamplePoints = 1u << 24;

// The points a square sample gives each of its 8 steps.
constexpr int kStepPoints = 8;

// C3. Wave samples play it on their root keys. Square samples play middle C there.
constexpr double kC3 = kMiddleC / 2;

// The rate a noise sample is rendered at, and its length.
constexpr uint32_t kNoiseRate = 65536;
constexpr uint32_t kNoisePoints = kNoiseRate;

} // namespace

bool SampleReadable(const Rom& rom, uint32_t address)
{
    const uint32_t length = rom.U32(address);
    const uint32_t loop_start = rom.U32(address + 8);
    const uint32_t loop_end = rom.U32(address + 12);
    const uint32_t points = loop_end ? loop_end : length;
    return points != 0 && points <= kMaxSamplePoints && rom.U32(address + 4) != 0 &&
           rom.Contains(address + 16, points) && (!loop_end || loop_start < loop_end);
}

int SampleRelease(uint8_t release)
{
    const double per_frame = -20.0 * std::log10((release + 0xE6) / 512.0);
    return Timecents(100.0 / per_frame / kFrameRate);
}

SoundfontBuilder::SoundfontBuilder(const Rom& rom, double psg_share, int songs)
    : rom_(rom), psg_share_(psg_share), banks_(songs)
{
}

int SoundfontBuilder::GameSample(uint32_t address)
{
    const auto cached = game_samples_.find(address);
    if (cached != game_samples_.end())
    {
        return cached->second;
    }

    // A sample has a header of its length, rate, loop start and loop end, then its 8-bit points. The driver plays up to
    // the loop's end and goes back to its start, or without a loop up to the length.
    const uint32_t length = rom_.U32(address);
    const uint32_t rate = rom_.U32(address + 4);
    const uint32_t loop_start = rom_.U32(address + 8);
    const uint32_t loop_end = rom_.U32(address + 12);
    const uint32_t points = loop_end ? loop_end : length;
    if (!SampleReadable(rom_, address))
    {
        game_samples_[address] = -1;
        return -1;
    }

    sf2::Sample s;
    s.name = AddressName("Sample", address);
    s.sample_rate = rate;
    s.original_key = 60;
    s.data.reserve(points + kMinLoop + kLoopGuard);
    for (uint32_t i = 0; i < points; i++)
    {
        s.data.push_back(int16_t(rom_.S8(address + 16 + i) * 256));
    }

    // A short loop is repeated until it's long enough, and the points after it repeat its start. The data ends at the
    // loop's end.
    if (loop_end)
    {
        s.loop_start = loop_start;
        s.loop_end = loop_end;
        LengthenLoop(s);
        AddLoopGuard(s);
    }

    file_.samples.push_back(std::move(s));
    game_samples_[address] = int(file_.samples.size()) - 1;

    return game_samples_[address];
}

int SoundfontBuilder::SquareSample(int duty)
{
    duty &= 3;
    const auto cached = squares_.find(duty);
    if (cached != squares_.end())
    {
        return cached->second;
    }

    sf2::Sample s;
    s.name = SquareName(duty);
    s.sample_rate = uint32_t(std::lround(8 * kStepPoints * kMiddleC));
    s.original_key = 60;
    SetCycle(s, CenteredSquareCycle(duty, kStepPoints, psg_share_));
    file_.samples.push_back(std::move(s));
    squares_[duty] = int(file_.samples.size()) - 1;

    return squares_[duty];
}

int SoundfontBuilder::WaveSample(uint32_t address)
{
    const auto cached = waves_.find(address);
    if (cached != waves_.end())
    {
        return cached->second;
    }

    sf2::Sample s;
    s.name = AddressName("Wave", address);
    s.sample_rate = uint32_t(std::lround(32 * kC3));
    s.original_key = 48;
    SetCycle(s, WavePatternCycle(rom_, address, psg_share_));
    file_.samples.push_back(std::move(s));
    waves_[address] = int(file_.samples.size()) - 1;

    return waves_[address];
}

int SoundfontBuilder::NoiseSample(uint8_t nr43)
{
    const auto cached = noises_.find(nr43);
    if (cached != noises_.end())
    {
        return cached->second;
    }

    sf2::Sample s;
    char name[32];
    std::snprintf(name, sizeof name, "Noise %02X", unsigned(nr43));
    s.name = name;
    s.sample_rate = kNoiseRate;
    s.original_key = 60;

    // Each point at 65536 Hz moves the LFSR on by 8 steps of its clock with a divisor of 1.
    AppendNoise(s, nr43, kNoisePoints, 524288 / kNoiseRate, psg_share_);
    s.loop_start = 0;
    s.loop_end = uint32_t(s.data.size());
    AddLoopGuard(s);

    file_.samples.push_back(std::move(s));
    noises_[nr43] = int(file_.samples.size()) - 1;

    return noises_[nr43];
}

int SoundfontBuilder::AddInstrument(const std::string& name, std::vector<sf2::Zone> zones)
{
    file_.instruments.push_back(MakeInstrument(name, std::move(zones)));

    return int(file_.instruments.size()) - 1;
}

void SoundfontBuilder::AddPreset(const std::string& name, int bank, int program, int instrument)
{
    file_.presets.push_back(MakePreset(name, uint16_t(bank), uint16_t(program), instrument));
}

} // namespace agblib::sequence_export::rd2
