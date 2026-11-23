// SPDX-License-Identifier: MIT

// The mixer of webfoot::Player: the mix of a frame's samples in the driver's blocks, and each voice's points.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/webfoot/driver.h"
#include "agblib/webfoot/player.h"
#include "player_internal.h"

namespace agblib::webfoot
{

namespace
{

// The place of a pan's centre.
constexpr int kPanCentre = 64;

// The shift that brings a level times the music's volume and the pan into a side's level.
constexpr int kLevelShift = 10;

// The shift of the sum of a side's points times their levels into a FIFO's byte.
constexpr int kOutputShift = 6;

// The samples that the mixer's routine adds when it's asked for none: its loop runs once before it checks the count.
constexpr int32_t k2002EmptyCount = 2;
constexpr int32_t k2003EmptyCount = 8;

} // namespace

void Player::Mix(uint32_t samples, std::vector<int8_t>* out)
{
    std::vector<uint32_t> sums;
    while (samples > 0 && playing_)
    {
        // The mixer's routine (0x0800A54E) runs a tick whenever the tick's samples are all mixed.
        if (samples_left_ == 0)
        {
            Tick();
            if (!playing_)
            {
                break;
            }

            samples_left_ = tick_samples_;
            elapsed_ = mixed_;
            if (on_step_)
            {
                on_step_(row_read_);
            }

            if (samples_left_ == 0)
            {
                Warn("a tick lasts no samples");
                playing_ = false;
                break;
            }

            continue;
        }

        // The mix routine mixes the handler's runs in blocks, and splits a block at each tick. The turns of the
        // ping-pong loops depend on where each piece starts.
        const uint32_t in_run = uint32_t(mixed_ % mix_samples_);
        const uint32_t block_left = std::min(mixer_.block - in_run % mixer_.block, mix_samples_ - in_run);
        const uint32_t count = std::min({samples, samples_left_, block_left});
        sums.assign(count + uint32_t(k2003EmptyCount), 0);
        for (int c = 0; c < kChannels; c++)
        {
            MixVoice(c, count, sums);
        }

        if (out)
        {
            // The routine at 0x08008A60 takes each side's byte from its sum, and clips it.
            for (uint32_t i = 0; i < count; i++)
            {
                const uint32_t w = sums[i];
                const int32_t left = int32_t(int16_t(uint16_t(w))) >> kOutputShift;
                const int32_t right = int32_t(w) >> (16 + kOutputShift);
                out->push_back(int8_t(std::clamp(left, -128, 127)));
                out->push_back(int8_t(std::clamp(right, -128, 127)));
            }
        }

        samples_left_ -= count;
        samples -= count;
        mixed_ += count;
    }
}

void Player::UpdateVoice(int c)
{
    // The voice routine (0x0800A748).
    Voice& v = voices_[size_t(c)];
    if (!(v.flags & kVoiceOn))
    {
        return;
    }

    if (v.flags & kVoiceLevels)
    {
        const int32_t level = int32_t(music_volume_) * v.volume;
        const int32_t left = (level * (kPanCentre - v.pan)) >> kLevelShift;
        const int32_t right = (level * (kPanCentre + v.pan)) >> kLevelShift;
        if (mixer_.byte_levels)
        {
            v.left = uint8_t(left);
            v.right = uint8_t(right);
        }
        else
        {
            v.left = uint16_t(left);
            v.right = uint16_t(right);
        }
    }

    if (v.flags & kVoiceSample)
    {
        const Sample s = ReadSample(rom_, v.sample);
        v.points = s.points;
        v.loop_start = int32_t(uint32_t(s.loop_start) << kFractionBits);
        v.end = int32_t(uint32_t(s.length) << kFractionBits);
        v.rate = s.rate;
        v.sample_flags = uint8_t(s.flags);
    }

    if (v.flags & kVoiceRestart)
    {
        v.position = 0;
        v.sample_flags &= uint8_t(~kVoiceBackwards);
    }

    if (v.flags & kVoicePitch)
    {
        const uint32_t speed = (Mul(Mul(v.rate, v.pitch) >> kFractionBits, mixer_.step_factor) + 0x7FF) >> 12;
        v.speed = speed;
        v.step = (v.sample_flags & kVoiceBackwards) ? -int32_t(speed) : int32_t(speed);
    }

    v.flags = kVoiceOn;
}

void Player::AddPoints(const Voice& v, int32_t count, uint32_t place, std::vector<uint32_t>& sums)
{
    // The mixer's routines in IWRAM (0x03003D44 on in the 2002 build): each sample adds its point times the levels to
    // the sums of the two sides. They take the point at the place's whole part, without interpolation.
    if (count < 0)
    {
        Warn("the mixer was asked for fewer than no samples");
        return;
    }

    if (count == 0)
    {
        count = mixer_.byte_levels ? k2002EmptyCount : k2003EmptyCount;
    }

    int32_t position = v.position;
    const uint32_t packed = uint32_t(v.left) | (uint32_t(v.right) << 16);
    for (int32_t i = 0; i < count; i++, position += v.step)
    {
        if (place + uint32_t(i) >= sums.size())
        {
            break;
        }

        const int32_t point = rom_.S8(v.points + uint32_t(position >> kFractionBits));
        uint32_t& w = sums[place + uint32_t(i)];
        if (mixer_.byte_levels)
        {
            // Two halfwords, each of which wraps by itself.
            const uint16_t left = uint16_t(uint16_t(w) + point * int32_t(v.left));
            const uint16_t right = uint16_t(uint16_t(w >> 16) + point * int32_t(v.right));
            w = left | (uint32_t(right) << 16);
        }
        else
        {
            // One word. The left side's sum carries into the right's.
            w += uint32_t(point) * packed;
        }
    }
}

void Player::MixVoice(int c, uint32_t samples, std::vector<uint32_t>& sums)
{
    // The routine that mixes a voice (0x0800A80E): it splits the samples at the sample's end and loop.
    Voice& v = voices_[size_t(c)];
    if (!(v.flags & kVoiceOn))
    {
        return;
    }

    int32_t left = int32_t(samples);
    uint32_t place = 0;
    const int32_t speed = int32_t(v.speed);
    const auto until = [&](int32_t distance) -> int32_t
    {
        if (speed == 0)
        {
            Warn("a voice with a step of 0 reached its sample's end");
            return 0;
        }

        return (distance + speed - 1) / speed;
    };

    // The driver's mixer loops for ever where a loop's turn is behind the voice's place, as after an offset into a
    // ping-pong loop that plays backwards, or where a step is longer than a loop. Each turn of a voice that plays on
    // takes at least one sample, or one turn of each way.
    int32_t turns = 2 * left + 2;
    const auto hangs = [&](int32_t k)
    {
        if (k >= 0 && --turns > 0)
        {
            return false;
        }

        Warn("a voice's place doesn't reach its loop's turn: the driver's mixer stops for ever here");
        playing_ = false;
        v.flags = 0;

        return true;
    };

    if (!(v.sample_flags & kSampleLoops))
    {
        const int32_t end = v.position + v.step * left;
        if (v.end > end)
        {
            AddPoints(v, left, place, sums);
            v.position = end;
            return;
        }

        AddPoints(v, until(v.end - v.position), place, sums);
        v.flags = 0;
        channels_[size_t(c)].flags &= uint8_t(~kChannelSounding);
        return;
    }

    if (!(v.sample_flags & kSamplePingPong))
    {
        while (true)
        {
            const int32_t end = v.position + v.step * left;
            if (v.end > end)
            {
                AddPoints(v, left, place, sums);
                v.position = end;
                return;
            }

            const int32_t k = until(v.end - v.position);
            if (hangs(k))
            {
                return;
            }

            const int32_t next = v.position + speed * k - (v.end - v.loop_start);
            AddPoints(v, k, place, sums);
            place += uint32_t(k);
            left -= k;
            v.position = next;
            if (left <= 0 || speed == 0)
            {
                return;
            }
        }
    }

    // A ping-pong loop. At the loop's end, the place turns to 2 * end - place - the steps, less 1 in the 2003 build
    // (0x0801FB1A in The Legacy of Goku II). At the loop's start, the 2002 build takes the place as 2 * start - place -
    // the steps, where a reflection would add the steps. The 2003 build adds them (0x0801FAC2).
    while (true)
    {
        const int32_t end = v.position + v.step * left;
        int32_t k = 0, next = 0;
        if (v.sample_flags & kVoiceBackwards)
        {
            if (!(v.loop_start > end))
            {
                AddPoints(v, left, place, sums);
                v.position = end;
                return;
            }

            k = until(v.position - v.loop_start);
            if (hangs(k))
            {
                return;
            }

            next = build_ == Build::k2002 ? 2 * v.loop_start - v.position - speed * k
                                          : 2 * v.loop_start - v.position + speed * k;
            AddPoints(v, k, place, sums);
            v.sample_flags &= uint8_t(~kVoiceBackwards);
            v.step = speed;
        }
        else
        {
            if (v.end > end)
            {
                AddPoints(v, left, place, sums);
                v.position = end;
                return;
            }

            k = until(v.end - v.position);
            if (hangs(k))
            {
                return;
            }

            next = 2 * v.end - v.position - speed * k - (build_ == Build::k2002 ? 0 : 1);
            AddPoints(v, k, place, sums);
            v.sample_flags |= kVoiceBackwards;
            v.step = -speed;
        }

        place += uint32_t(k);
        left -= k;
        v.position = next;
        if (left <= 0 || speed == 0)
        {
            return;
        }
    }
}

} // namespace agblib::webfoot
