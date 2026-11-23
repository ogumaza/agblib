// SPDX-License-Identifier: MIT

#include "agblib/sequence_export/webfoot/soundfont.h"

#include <cstdint>

#include "agblib/agb/rom.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/webfoot/driver.h"

namespace agblib::sequence_export::webfoot
{

using agblib::sequence_export::agb::AddLoopGuard;
using agblib::sequence_export::agb::LengthenLoop;
using agblib::webfoot::kSampleLoops;
using agblib::webfoot::kSamplePingPong;
namespace sf2 = seqlib::sf2;

namespace
{

// 2^26: the driver's step factor is this over its nominal rate.
constexpr double kStepScale = 67108864.0;

// Returns true if the mixer loops `sample`.
bool Loops(const PlayedSample& sample)
{
    return (sample.flags & kSampleLoops) && sample.loop_start < sample.length;
}

// Returns the SoundFont sample of `played`: its points, signed 8-bit, and its loop.
sf2::Sample MakeSample(const Rom& rom, const PlayedSample& played, const char* name)
{
    sf2::Sample s;
    s.name = name;
    s.sample_rate = played.rate;
    s.original_key = uint8_t(kRootKey);
    const auto add = [&](uint32_t point)
    {
        s.data.push_back(int16_t(rom.S8(played.points + point) * 256));
    };

    for (uint32_t i = 0; i < played.length; i++)
    {
        add(i);
    }

    // The mixer turns a ping-pong loop at the edges of its first and last points. Each of them plays on both sides of
    // the turn.
    if (Loops(played))
    {
        s.loop_start = played.loop_start;
        if (PingPong(played))
        {
            for (uint32_t i = played.length; i > played.loop_start; i--)
            {
                add(i - 1);
            }
        }

        s.loop_end = uint32_t(s.data.size());
        LengthenLoop(s);
        AddLoopGuard(s);
    }

    if (s.data.empty())
    {
        s.data.push_back(0);
    }

    return s;
}

} // namespace

double PlayedRate(uint16_t rate, const Mixer& mixer)
{
    return double(rate) * mixer.step_factor * mixer.Rate() / kStepScale;
}

bool PingPong(const PlayedSample& sample)
{
    return Loops(sample) && (sample.flags & kSamplePingPong);
}

int InstrumentSet::Program(const PlayedSample& sample, int number)
{
    return instruments_.Program(sample, number);
}

Sf2File InstrumentSet::Build(const Rom& rom) const
{
    const auto make_sample = [&](const PlayedSample& played, const char* name)
    {
        return MakeSample(rom, played, name);
    };

    return instruments_.Build(make_sample);
}

} // namespace agblib::sequence_export::webfoot
