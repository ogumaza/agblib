// SPDX-License-Identifier: MIT

// The mixer of MP2K in Camelot's games: its synth voices, its steps through a sample's points, its channels and its
// output stage with the echo.

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "agblib/mp2k/sequencer.h"
#include "sequencer_shared.h"

namespace agblib::mp2k
{

bool Sequencer::CamelotSynthPoints(Channel& c, uint32_t step, int32_t* points, int count) const
{
    const int type = rom_.S8(c.position + 1);
    if (type == 0)
    {
        // A pulse wave's duty follows a triangle wave, at the top of the count. The wave is high below the duty
        // (0x03000eec).
        c.count = int32_t(uint32_t(c.count) + (uint32_t(rom_.U8(c.position + 3)) << 24));
        uint32_t phase = uint32_t(c.count) + (uint32_t(rom_.U8(c.position + 5)) << 24);
        if (int32_t(phase) < 0)
        {
            phase = ~phase;
        }

        const uint32_t duty = (phase >> 8) * rom_.U8(c.position + 4) + (uint32_t(rom_.U8(c.position + 2)) << 24);
        for (int i = 0; i < count; i++)
        {
            points[i] = c.fraction < duty ? 64 : -64;
            c.fraction += step << 3;
        }

        return false;
    }

    if (type == 1)
    {
        // A saw wave goes through its filter.
        int32_t filter = c.count;
        for (int i = 0; i < count; i++)
        {
            c.fraction += step << 3;
            filter = FilterSaw(SawPoint(c.fraction), filter);
            points[i] = filter;
        }

        c.count = filter;
        return true;
    }

    // A triangle wave (0x03001044).
    for (int i = 0; i < count; i++)
    {
        c.fraction += step << 3;
        const int32_t phase = int32_t(c.fraction);
        points[i] = phase >= 0 ? (phase >> 23) - 128 : 384 - int32_t(c.fraction >> 23);
    }

    return false;
}

bool Sequencer::PlaysCamelotSteps(const Channel& c) const
{
    return info_.camelot_adpcm && (c.type & kVoiceFixed) && (c.count < 0 || (c.type & kVoiceCompressed));
}

const std::vector<float>& Sequencer::CamelotStepPoints(const Wave& wave)
{
    std::shared_ptr<const std::vector<float>>& known = camelot_step_points_[wave.address];
    if (known)
    {
        return *known;
    }

    const std::vector<int32_t> points = DecodeCamelotSteps(rom_, wave, uint32_t(info_.samples_per_frame));
    known = std::make_shared<const std::vector<float>>(points.begin(), points.end());
    return *known;
}

void Sequencer::MixCamelot(Channel& c, const Wave& wave, uint32_t volumes, int meter)
{
    // The addresses below are those of the mixer's code in Golden Sun: The Lost Age, in IWRAM. The mixer counts the
    // channels that it mixes in a frame (0x0300088c). Each point adds the volumes times the point to the point's sums,
    // in a word that wraps round at 32 bits. A negative right side borrows from the left.
    camelot_channels_++;
    const int points = info_.samples_per_frame;
    uint32_t* sums = camelot_sums_.data();
    int16_t* meter_right = meter >= 0 ? &sums_[size_t(2 * meter * points)] : nullptr;
    int16_t* meter_left = meter_right ? meter_right + points : nullptr;
    const auto add = [&](int i, uint32_t volume, int32_t value)
    {
        sums[i] += volume * uint32_t(value);
        if (meter_right)
        {
            meter_right[i] = int16_t(meter_right[i] + int32_t((int64_t(volume & 0xFFFF) * value) >> 7));
            meter_left[i] = int16_t(meter_left[i] + int32_t((int64_t(volume >> 16) * value) >> 7));
        }
    };

    // A looped sample goes back to its loop's start at its end. A sample without a loop stops the channel there, and
    // so does a loop of no length (0x0300085a).
    const uint32_t loop_start = wave.Data() + wave.loop_start;
    const int32_t loop_length = (c.status & kStatusLoop) ? int32_t(wave.size - wave.loop_start) : 0;

    // A fixed-pitch sample plays a point for each point of the output, at the channel's volumes (0x03000c1c).
    if (c.type & kVoiceFixed)
    {
        // A mixer with 4-bit steps plays a sample of a negative size in steps, a byte for each two points. Such a
        // sample has no loop. The channel's fraction keeps the steps' state between frames: the index in its low bits
        // and the point above them. The voice's type marks the channel (0x03000c1c to 0x03000da0 in Mario Tennis: Power
        // Tour). A negative count takes the mark and the first state.
        if (PlaysCamelotSteps(c))
        {
            uint32_t state = c.fraction;
            if (c.count < 0)
            {
                c.type |= kVoiceCompressed;
                c.count = -c.count;
                state = kCamelotStepsStart;
            }

            int32_t point = int32_t(state) >> 16;
            uint32_t index = state & 0xFF;
            const int mixed = std::min(c.count, points);
            c.count -= mixed;
            int i = 0;
            do
            {
                const uint32_t byte = uint32_t(rom_.S8(c.position++));
                CamelotStep(byte << 24 & 0xF0000000u, point, index);
                add(i, volumes, point);
                CamelotStep(byte << 28, point, index);
                add(i + 1, volumes, point);
                i += 2;
            } while (i < mixed);

            c.fraction = index | uint32_t(point) << 16;
            if (c.count == 0)
            {
                c.status = 0;
            }

            return;
        }

        for (int i = 0; i < points; i++)
        {
            add(i, volumes, rom_.S8(c.position));
            c.position++;
            if (--c.count == 0)
            {
                if (loop_length == 0)
                {
                    c.status = 0;
                    return;
                }

                c.count = loop_length;
                c.position = loop_start;
            }
        }

        return;
    }

    // A sample with no points left becomes a synth voice (0x030008a4). Its data follows at its position: 0x80, its
    // type, and a pulse wave's duty, speed, depth and offset. The phase moves on by 8 times a sample's step.
    const uint32_t step = c.frequency * info_.step_scale;
    if (c.synth || c.count == 0)
    {
        // A saw wave plays at half the volumes, rounded down on each side (0x03000fc0).
        c.synth = true;
        std::array<int32_t, kMostFramePoints> synth_points;
        const bool saw = CamelotSynthPoints(c, step, synth_points.data(), points);
        const uint32_t volume = saw ? (volumes >> 1) & ~0xFF00u : volumes;
        for (int i = 0; i < points; i++)
        {
            add(i, volume, synth_points[size_t(i)]);
        }

        return;
    }

    // Other samples play at each side's volume halved, rounded up (0x030008d0). Each point is between two of the
    // sample's points, at the fraction in 2^-23 points.
    uint32_t volume = ((volumes >> 1) + 0x8000 + (volumes & 1)) & ~0xFF00u;
    uint32_t fraction = c.fraction;
    int32_t count = c.count;
    uint32_t next = c.position + 1; // the address of the next point
    int32_t point = rom_.S8(c.position);
    int32_t difference = rom_.S8(next) - point;

    // When the sample has more points left than the frame takes, the mixer runs a loop of 8 points with the code of
    // its step: below 1, from 1 to 2, or from 2 up (0x03000940). The code below 1 keeps the fraction less 1 and
    // interpolates to 9 bits. The others interpolate to 8 bits at twice the volumes. The first point of each 8
    // multiplies the fraction by the difference. Each later one does so unless the step has just gone on to a point
    // whose difference is 0. The point then takes the last point's result, shifted right by 22 or 23 bits: 0, or -1
    // when it was negative.
    const uint64_t frame_steps = fraction + uint64_t(step) * uint64_t(points);
    const uint32_t advance = uint32_t(frame_steps >> 23);
    if (count > int32_t(advance))
    {
        enum class Code
        {
            kBelowOne,
            kOneToTwo,
            kTwoUp,
        };
        Code code = Code::kBelowOne;
        uint32_t increment = step - 0x800000;
        if (int32_t(increment) >= 0)
        {
            volume <<= 1;
            code = int32_t(step - 0x1000000) < 0 ? Code::kOneToTwo : Code::kTwoUp;
            increment = code == Code::kOneToTwo ? step - 0x1000000 : step - 0x800000;
        }

        uint32_t product = 0;
        bool multiply = true;
        for (int i = 0; i < points; i++)
        {
            if (i % 8 == 0 || multiply)
            {
                product = fraction * uint32_t(difference);
            }

            const int32_t value =
                code == Code::kBelowOne ? (int32_t(product) >> 22) + 2 * point : point + (int32_t(product) >> 23);
            product = uint32_t(value);
            add(i, volume, value);

            fraction += increment;
            if (code == Code::kBelowOne)
            {
                multiply = true;
                if (int32_t(fraction) >= 0)
                {
                    point += difference;
                    next++;
                    difference = rom_.S8(next) - point;
                    multiply = difference != 0;
                }
            }
            else
            {
                if (code == Code::kOneToTwo)
                {
                    point += difference;
                    if (int32_t(fraction) >= 0)
                    {
                        next++;
                        point = rom_.S8(next);
                    }
                }
                else
                {
                    next += fraction >> 23;
                    point = rom_.S8(next);
                }

                next++;
                difference = rom_.S8(next) - point;
                multiply = difference != 0;
            }

            fraction &= 0x7FFFFF;
        }

        c.fraction = fraction;
        c.count = count - int32_t(advance);
        c.position += advance;
        return;
    }

    // Otherwise each point goes on by the step, and the sample's end loops or stops the channel (0x03000b44). A loop
    // goes back to its start, past it by the points that the step overshot.
    for (int i = 0; i < points; i++)
    {
        add(i, volume, (int32_t(fraction * uint32_t(difference)) >> 22) + 2 * point);
        fraction += step;
        const uint32_t steps = fraction >> 23;
        if (steps == 0)
        {
            continue;
        }

        count -= int32_t(steps);
        if (count <= 0)
        {
            if (loop_length == 0)
            {
                c.status = 0;
                return;
            }

            const int32_t overshoot = -count;
            count += loop_length;
            next = loop_start + uint32_t(overshoot);
            point = rom_.S8(next);
        }
        else if (steps == 1)
        {
            point += difference;
        }
        else
        {
            next += steps - 1;
            point = rom_.S8(next);
        }

        next++;
        difference = rom_.S8(next) - point;
        fraction &= ~0x3F800000u;
    }

    c.fraction = fraction;
    c.count = count;
    c.position = next - 1;
}

void Sequencer::OutputCamelot()
{
    // The output stage (0x03000d40) writes two points for each sum on each side, or one in Mario Tennis: Power Tour
    // (0x03000dec). The two sides' sums share a word, 16 bits each, and the output stage works on both halves at once.
    // A sum that doesn't fit in 15 bits saturates (0x03000e90).
    constexpr uint32_t kSigns = 0x80008000;
    constexpr uint32_t kRound = kSigns >> 9;
    constexpr uint32_t kBytes = 0xFF00FF00;
    const auto rotate = [](uint32_t value, int bits)
    {
        return value >> bits | value << (32 - bits);
    };

    const auto saturate = [](uint32_t sum)
    {
        const uint32_t swapped = sum >> 16 | sum << 16;
        if ((swapped ^ swapped << 1) & 0x80000000)
        {
            const bool high = sum & 0x4000;
            sum = (sum | 0x4000) & ~0x3FC0u;
            if (high)
            {
                sum -= 0x40;
            }
        }

        if ((sum ^ sum << 1) & 0x80000000)
        {
            const bool high = sum & 0x40000000;
            sum = (sum | 0x40000000) & ~0x3FC00000u;
            if (high)
            {
                sum -= 0x400000;
            }
        }

        return sum;
    };

    // The points of both halves in a word's bytes: the left side's first and second points, then the right side's. A
    // negative half takes 64 more first. The first point is bits 7 to 14 of the half, and the second those of the half
    // less 64.
    const auto points = [&](uint32_t sum)
    {
        const uint32_t up = sum + ((sum & kSigns) >> 9);
        return (rotate(up - kRound, 15) & kBytes) | (rotate(up, 15) & kBytes) >> 8;
    };

    // The sums take 64 more before they turn into points. With 4 channels or more in the frame, only the first two do.
    // The stage writes the jump back of its loop to match (0x03000d4c). It also shifts the count up by 4 bits for the
    // next frame. Only the next frame's count reaches the low 4 bits that the stage reads.
    const bool many = (camelot_channels_ & 12) != 0;
    camelot_channels_ = 0;

    // The echo of the left side takes the right side's points from camelot_tap points before: the last frame's last
    // part, then the frame's first points.
    const uint32_t frame_points = uint32_t(info_.samples_per_frame);
    const uint32_t tap = uint32_t(info_.camelot_tap);
    const uint32_t scale = uint32_t(PointsPerMixed());
    const uint32_t size = scale * kPcmBufferSize;
    int8_t* right = pcm_.data();
    int8_t* left = right + size;
    uint32_t place = part_;
    uint32_t echo = camelot_last_part_ + scale * (frame_points - tap);
    camelot_last_part_ = part_;

    // Each side of the next frame's sums starts with a share of its old points, and a share of the other side's. The
    // constants spread over both halves.
    const uint32_t echo_right = uint32_t(info_.camelot_cross_echo) << 16 | uint32_t(info_.camelot_echo);
    const uint32_t echo_left = (uint32_t(info_.camelot_echo) << 16) + uint32_t(info_.camelot_cross_echo);
    for (uint32_t i = 0; i < frame_points; i += 2)
    {
        if (i == tap)
        {
            echo = part_;
        }

        uint32_t first = camelot_sums_[i];
        uint32_t second = camelot_sums_[i + 1];
        if (i == 0 || !many)
        {
            first += kRound;
            second += kRound;
        }

        if (((first ^ first << 1) | (second ^ second << 1)) & kSigns)
        {
            first = saturate(first);
            second = saturate(second);
        }

        // With one point for each sum, a word has the left side's point in its low byte and the right side's above it.
        // The sums start with shares of the buffer's old points, a quarter of the frame's right side and a quarter of
        // the right side's tap. A half takes 64 less, or 64 more when it's negative.
        if (scale == 1)
        {
            const int32_t first_word = int32_t(rotate(first, 16)) >> 7;
            const int32_t second_word = int32_t(rotate(second, 16)) >> 7;
            uint32_t first_echo = uint32_t(right[place]) * echo_right + uint32_t(left[place]) * echo_left;
            uint32_t second_echo = uint32_t(right[place + 1]) * echo_right + uint32_t(left[place + 1]) * echo_left;
            left[place] = int8_t(first_word);
            left[place + 1] = int8_t(second_word);
            const int32_t first_right = first_word >> 16;
            const int32_t second_right = second_word >> 16;
            right[place] = int8_t(first_right);
            right[place + 1] = int8_t(second_right);
            place += 2;

            first_echo += uint32_t(first_right) << 5;
            second_echo += uint32_t(second_right) << 5;
            first_echo += uint32_t(right[echo]) << 21;
            second_echo += uint32_t(right[echo + 1]) << 21;
            echo += 2;
            camelot_sums_[i] = first_echo - kRound + ((first_echo & kSigns) >> 8);
            camelot_sums_[i + 1] = second_echo - kRound + ((second_echo & kSigns) >> 8);
            continue;
        }

        const uint32_t first_points = points(first);
        const uint32_t second_points = points(second);

        // The shares come from the first points of the buffer's part: 3 frames before this one in Golden Sun: The Lost
        // Age.
        uint32_t first_echo = uint32_t(right[place]) * echo_right + uint32_t(left[place]) * echo_left;
        uint32_t second_echo = uint32_t(right[place + 2]) * echo_right + uint32_t(left[place + 2]) * echo_left;
        left[place] = int8_t(first_points);
        left[place + 1] = int8_t(first_points >> 8);
        left[place + 2] = int8_t(second_points);
        left[place + 3] = int8_t(second_points >> 8);
        right[place] = int8_t(first_points >> 16);
        right[place + 1] = int8_t(first_points >> 24);
        right[place + 2] = int8_t(second_points >> 16);
        right[place + 3] = int8_t(second_points >> 24);
        place += 4;

        // The right side takes a quarter of its second points from this frame, plus 1/128 of that. The left side takes
        // a quarter of the right side's first points from camelot_tap points before (0x03000e34).
        uint32_t quarter = ((second_points & 0xFFFF0000) | first_points >> 16) & kBytes;
        quarter += quarter >> 7;
        second_echo += uint32_t(int32_t(quarter) >> 19);
        first_echo += uint32_t(int32_t(rotate(quarter, 16)) >> 19);
        first_echo += uint32_t(int32_t(right[echo])) << 21;
        second_echo += uint32_t(int32_t(right[echo + 2])) << 21;
        echo += 4;
        camelot_sums_[i] = first_echo;
        camelot_sums_[i + 1] = second_echo;
    }
}

} // namespace agblib::mp2k
