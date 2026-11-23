// SPDX-License-Identifier: MIT

// The enhanced mixer of Konami's output stage: its voices at a higher rate, their gains and the echo buses.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "ntdlib/wave/adpcm.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/sinc_mix.h"
#include "agblib/konami/driver.h"
#include "agblib/konami/output.h"
#include "agblib/konami/sequencer.h"
#include "output_impl.h"

namespace agblib::konami
{

void Output::Impl::MeterPart(int voice, int fifo, float part)
{
    float& peak = voice_peaks[size_t(voice + 4)][size_t(fifo)];
    peak = std::max(peak, std::fabs(part));
}

void Output::Impl::StartEnhanced(int voice, uint32_t data, uint32_t size, int32_t loop, bool coded, bool reverse)
{
    EnhancedVoice& e = enhanced->voices[size_t(voice)];
    e = EnhancedVoice();
    e.data = data;
    e.size = size;
    e.loops = loop >= 0 && !reverse && uint32_t(loop) < size;
    e.loop_start = e.loops ? uint32_t(loop) : 0;
    e.reverse = reverse;
    e.starting = true;
    e.position = uint64_t(voices[size_t(voice)].fraction) << 20;
    if (coded)
    {
        e.decoded = uint16_t(DecodedSampleOf(data, size, loop) + 1);
    }
}

int Output::Impl::DecodedSampleOf(uint32_t data, uint32_t size, int32_t loop)
{
    std::vector<std::shared_ptr<const DecodedSample>>& decoded = enhanced->decoded;
    for (size_t i = 0; i < decoded.size(); i++)
    {
        if (decoded[i]->start == data)
        {
            return int(i);
        }
    }

    // The console's decoder takes each point in turn. A new pass starts at the loop's byte, with the decoder as the
    // first pass left it.
    const auto decode = [this](uint32_t from, uint32_t points, OutputVoice& v, std::vector<int8_t>& out)
    {
        uint32_t position = from;
        out.resize(points);
        for (uint32_t k = 0; k < points; k++)
        {
            out[k] = int8_t(NextCoded(v, position, k == 0 ? 0 : 1));
        }
    };

    auto sample = std::make_shared<DecodedSample>();
    sample->start = data;
    OutputVoice state;
    state.start = data;
    decode(data, size, state, sample->first);
    if (loop >= 0 && uint32_t(loop) < size)
    {
        decode(data + uint32_t(loop), size - uint32_t(loop), state, sample->loop);
    }

    decoded.push_back(std::move(sample));
    return int(decoded.size()) - 1;
}

float Output::Impl::Gain(Scale scale, int level) const
{
    switch (scale)
    {
    case Scale::kRoundUp7:
    case Scale::kDown7:
        return float(level) / 128.0f;
    case Scale::kUp4:
    case Scale::kDown4:
        return float(level) / 16.0f;
    case Scale::kTable:
    case Scale::kTableSigned:
        {
            const uint32_t row = uint32_t(rules.level_table) + uint32_t(level) * 256;
            return float(rom.S8(row + 0x7F) - rom.S8(row + 0x80)) / 255.0f;
        }
    }

    return 0.0f;
}

float Output::Impl::PointOf(const EnhancedVoice& e, int64_t point) const
{
    if (point < 0)
    {
        return 0.0f;
    }

    uint64_t at = uint64_t(point);
    const uint64_t loop_length = e.size - e.loop_start;
    if (e.decoded)
    {
        const DecodedSample& sample = *enhanced->decoded[size_t(e.decoded - 1)];
        if (at < e.size)
        {
            return float(sample.first[size_t(at)]);
        }

        return e.loops ? float(sample.loop[size_t((at - e.size) % loop_length)]) : 0.0f;
    }

    if (at >= e.size)
    {
        if (!e.loops)
        {
            return 0.0f;
        }

        at = e.loop_start + (at - e.loop_start) % loop_length;
    }

    return float(rom.S8(e.reverse ? e.data + e.size - 1 - uint32_t(at) : e.data + uint32_t(at)));
}

void Output::Impl::MixEnhancedVoice(int voice, float* a, float* b, std::array<float, 2> to, uint32_t step,
                                    uint32_t period)
{
    EnhancedVoice& e = enhanced->voices[size_t(voice)];
    const std::array<float, 2> from = e.starting ? to : e.levels;
    e.levels = to;
    e.starting = false;

    // A sample's position moves by the step for each of the console's points that the frame's time contains.
    const uint64_t move = (uint64_t(step) * agb::kFrameCycles << 20) / period;
    const uint64_t start = e.position;
    e.position += move;
    const bool heard = from[0] != 0.0f || from[1] != 0.0f || to[0] != 0.0f || to[1] != 0.0f;
    if (!enhanced->frames || !heard || Muted(voice))
    {
        return;
    }

    agb::MixSincRamp(
        start, move, agb::kEnhancedFrameSamples, from, to, a, b, [&](int64_t n) { return PointOf(e, n); },
        [&](float part_a, float part_b)
        {
            if (metering)
            {
                MeterPart(voice, 0, part_a);
                MeterPart(voice, 1, part_b);
            }
        });
}

void Output::Impl::MixEnhancedBus(int bus, const OutputEcho& e)
{
    // A delay of no blocks reads the line's oldest block: the line's whole length.
    const size_t samples = agb::kEnhancedFrameSamples;
    const uint32_t blocks = e.delay_blocks ? e.delay_blocks : 256;
    const uint64_t delay = (uint64_t(blocks) * block_cycles * samples + agb::kFrameCycles / 2) / agb::kFrameCycles;
    std::vector<float>& line = enhanced->lines[size_t(bus)];
    const size_t length = enhanced->line_samples;
    size_t write = enhanced->line_position[size_t(bus)];
    const float feedback = float(e.feedback) / 256.0f;
    float* out_a = enhanced->out.data();
    float* out_b = out_a + samples;
    const float* in_a = enhanced->inputs[size_t(bus)].data();
    const float* in_b = in_a + samples;
    const auto keep = [](float value)
    {
        return std::fabs(value) < 1.0f / 65536.0f ? 0.0f : value;
    };

    for (size_t i = 0; i < samples; i++)
    {
        const size_t read = (write + length - size_t(delay)) % length;
        const float a = line[read] * feedback + in_a[i];
        const float b = line[length + read] * feedback + in_b[i];
        if (echo_heard)
        {
            line[write] = keep(a);
            line[length + write] = keep(b);
        }

        out_a[i] += a;
        out_b[i] += b;
        write = (write + 1) % length;
    }

    enhanced->line_position[size_t(bus)] = write;
}

void Output::Impl::MixEnhanced()
{
    const size_t samples = agb::kEnhancedFrameSamples;
    Enhanced& e = *enhanced;
    std::fill(e.out.begin(), e.out.end(), 0.0f);
    float* out_a = e.out.data();
    float* out_b = out_a + samples;
    if (stage == Stage::kFifo)
    {
        // Each FIFO's two voices move on at the FIFO's timer, a point for each overflow.
        for (int n = 0; n < voice_count; n++)
        {
            const OutputVoice& v = voices[size_t(n)];
            const int f = n / 2;
            const uint32_t period = FifoCycles(f);
            if (!(v.flags & kVoiceOn) || period == 0)
            {
                continue;
            }

            const int level = v.volume + 1;
            const float gain = level <= 1 ? 0.0f : float(level) / 32.0f;
            MixEnhancedVoice(n, out_a, out_b, {f == 0 ? gain : 0.0f, f == 1 ? gain : 0.0f}, 0x1000, period);
        }

        return;
    }

    const uint32_t period = block_cycles / kBlock;
    if (stage == Stage::kRing)
    {
        for (int n = 0; n < voice_count; n++)
        {
            const OutputVoice& v = voices[size_t(n)];
            if (!(v.flags & kVoiceOn) || v.volume == 0)
            {
                continue;
            }

            const float gain = Gain(Scale::kDown4, v.volume + 1);
            const bool on_a = n < kRingVoices;
            MixEnhancedVoice(n, out_a, out_b, {on_a ? gain : 0.0f, on_a ? 0.0f : gain}, v.step, period);
        }

        return;
    }

    // The console's mask sends each voice to the dry sums or to its bus's input.
    for (std::vector<float>& input : e.inputs)
    {
        std::fill(input.begin(), input.end(), 0.0f);
    }

    const uint32_t routing = rules.mask_each_block ? echo[0].routing | (echo[1].routing << 1) : echo_mask;
    uint32_t mask = routing;
    std::array<float, 2 * agb::kEnhancedFrameSamples> unused{};
    for (int n = voice_count - 1; n >= 0; n--)
    {
        const OutputVoice& v = voices[size_t(n)];
        const uint32_t choice = mask & 3;
        mask >>= 2;
        if (!(v.flags & kVoiceOn) || (rules.skips_silent && v.volume == 0))
        {
            continue;
        }

        // A path with one value for both sides takes the right side's level. A keeping build's path stays the note's.
        const Path path = PathOf(v);
        const Scale scale = path == Path::kReverse                          ? rules.reverse
                            : path == Path::kEqual || path == Path::kDouble ? rules.equal
                                                                            : rules.unequal;
        const float to_a = Gain(scale, v.right);
        const bool one_value =
            path == Path::kEqual || path == Path::kDouble || (path == Path::kReverse && !rules.reverse_two_levels);
        const float to_b = one_value ? to_a : Gain(scale, v.left);
        float* a = choice == 0 ? out_a : choice == 3 ? unused.data() : e.inputs[choice - 1].data();
        float* b = choice == 0 ? out_b : a + samples;
        MixEnhancedVoice(n, a, b, {to_a, to_b}, v.step, period);
    }

    if (!rules.has_echo)
    {
        return;
    }

    const uint32_t buses = routing >> 30;
    for (int bus = 0; bus < 2; bus++)
    {
        if (buses & (1u << bus))
        {
            MixEnhancedBus(bus, echo[size_t(bus)]);
        }
    }

    // A line rings on through a frame without the mode's mixing, without any of it reaching the output.
    if (!e.frames)
    {
        std::fill(e.out.begin(), e.out.end(), 0.0f);
    }
}

} // namespace agblib::konami
