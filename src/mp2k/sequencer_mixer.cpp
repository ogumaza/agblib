// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/wave/sinc.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/warnings.h"
#include "agblib/mp2k/commands.h"
#include "agblib/mp2k/driver.h"
#include "agblib/mp2k/sequencer.h"
#include "agblib/mp2k/song.h"
#include "sequencer_shared.h"

namespace agblib::mp2k
{
namespace
{

// Adds `value` times `volume`, over 256 and rounded down, to a byte of the mixer's output. The byte wraps round.
void AddPoint(int8_t& out, int32_t value, int volume)
{
    out = int8_t(uint8_t(out) + uint8_t((value * volume) >> 8));
}

} // namespace

void Sequencer::StartMixing()
{
    if (dma_period_ < 2)
    {
        throw std::invalid_argument("the mixer's buffer doesn't hold two frames' parts");
    }
    if (info_.camelot_mixer &&
        (info_.camelot_tap <= 0 || info_.camelot_tap >= info_.samples_per_frame || info_.camelot_tap % 2 != 0))
    {
        throw std::invalid_argument("the echo of Camelot's mixer doesn't reach back within a frame");
    }

    // Camelot's output stage can write two points for each of its mixer's points, into buffers twice as long.
    mixing_ = true;
    pcm_.assign(size_t(2 * kPcmBufferSize * PointsPerMixed()), 0);
    sums_.assign(size_t(2 * kMaxTracks * info_.samples_per_frame), 0);
    if (info_.camelot_mixer)
    {
        camelot_sums_.assign(size_t(info_.samples_per_frame), 0);
        camelot_channels_ = 0;
        camelot_last_part_ = 0;
    }
}

int Sequencer::PointsPerMixed() const
{
    return info_.camelot_mixer ? info_.camelot_output_points : 1;
}

std::span<const int8_t> Sequencer::Right() const
{
    if (!mixing_)
    {
        return {};
    }

    return std::span<const int8_t>(pcm_).subspan(part_, size_t(info_.samples_per_frame * PointsPerMixed()));
}

std::span<const int8_t> Sequencer::Left() const
{
    if (!mixing_)
    {
        return {};
    }

    const size_t size = kPcmBufferSize * size_t(PointsPerMixed());
    return std::span<const int8_t>(pcm_).subspan((info_.mono ? 0 : size) + part_,
                                                 size_t(info_.samples_per_frame * PointsPerMixed()));
}

void Sequencer::RunDirect()
{
    const bool metering = mixing_ && metering_;
    if (mixing_)
    {
        StartPart();
    }

    if (enhanced_)
    {
        StartEnhancedPart();
    }

    if (metering)
    {
        std::fill(sums_.begin(), sums_.end(), int16_t(0));
    }

    // The console's DirectSound channels, then the grown ones.
    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& c = channels_[size_t(i)];
        if ((i >= direct_channels_ && (i < kChannelCount || c.part)) || !(c.status & kStatusOn))
        {
            continue;
        }

        // The mixer starts a note's sample, then moves its envelope on: up by the attack each frame, down by the decay
        // to the sustain level, and down by the release after the note's release, each a fraction out of 256. After the
        // release, the note can carry on at its echo's level for the echo's length.
        Wave wave;
        ReadWave(rom_, c.wave, wave);
        int envelope = c.envelope;
        bool echo = false;

        // A step of the decay: down to the sustain level, where the sustain phase starts. At a sustain of 0, the note
        // goes on at its echo's level instead.
        const auto decay = [&]()
        {
            envelope = (envelope * c.decay) >> 8;
            if (envelope <= c.sustain)
            {
                envelope = c.sustain;
                if (envelope == 0)
                {
                    echo = true;
                }
                else
                {
                    c.status--;
                }
            }
        };

        const bool starting = (c.status & kStatusStart) != 0;
        if (starting)
        {
            if (c.status & kStatusStop)
            {
                c.status = 0;
                EndNote(i, false);
                continue;
            }

            // The enhanced mode counts the position from the sample's first point. The note starts c.count points in.
            enhanced_channels_[size_t(i)].position = uint64_t(uint32_t(c.count)) << 23;
            c.status = kPhaseAttack;
            c.position = wave.Data() + uint32_t(c.count);
            c.count = int32_t(wave.size - uint32_t(c.count));
            c.fraction = 0;
            envelope = 0;
            if (wave.loops)
            {
                c.status |= kStatusLoop;
            }
        }
        else if (c.status & kStatusEcho)
        {
            if (c.echo_length-- <= 1)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }
        }
        else if (c.status & kStatusStop)
        {
            // Camelot's mixer takes 256 less the release off the level each frame, in a straight line.
            envelope = info_.camelot_mixer ? envelope + c.release - 256 : (envelope * c.release) >> 8;
            echo = info_.metroid_sequencer ? envelope <= 0 : envelope <= c.echo_volume;
        }
        else if ((c.status & kStatusEnvelope) == kPhaseDecay)
        {
            decay();
        }

        if ((c.status & (kStatusStop | kStatusEcho | kStatusEnvelope)) == kPhaseAttack)
        {
            envelope += c.attack;
            if (envelope >= 255)
            {
                envelope = 255;
                c.status--;

                // The Metroid games' mixer moves the level down by the decay in the frame that the attack ends.
                if (info_.metroid_sequencer)
                {
                    decay();
                }
            }
        }

        // The Metroid games' mixer ends a note whose level reaches 0, unless it has both an echo volume and an echo
        // length. Such a note is silent for that frame, then plays on at its last level for the length less 1
        // (0x080010EA in Metroid Fusion).
        if (echo && info_.metroid_sequencer)
        {
            if (!c.echo_volume || !c.echo_length)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }

            c.status |= kStatusEcho;
            continue;
        }

        if (echo)
        {
            envelope = c.echo_volume;
            if (envelope == 0)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }

            c.status |= kStatusEcho;
        }

        c.envelope = uint8_t(envelope);
        CheckAudible(i);

        // A muted channel plays at volume 0.
        const int owner = owners_[size_t(i)];
        const bool muted = owner >= 0 && (muted_tracks_ >> owner & 1);
        const int meter = metering && !muted ? owner : -1;

        // Camelot's mixer plays each side at 9/8 of the envelope. It leaves out a note that comes out silent on both
        // sides. Such a note doesn't move on through its sample (0x03000846 to 0x03000888 in Golden Sun: The Lost Age).
        if (info_.camelot_mixer)
        {
            const int level = envelope + (envelope >> 3);
            const uint32_t right = uint32_t((c.right * level) >> 9);
            const uint32_t left = uint32_t((c.left * level) >> 9);
            if (right == 0 && left == 0)
            {
                enhanced_channels_[size_t(i)].volumes = {};
                continue;
            }

            MarkSounded(i);
            if (mixing_)
            {
                // The enhanced mode's volumes: each side's volume at 9/8 of the envelope, over 512, and the output
                // stage's 128 to a point.
                if (enhanced_)
                {
                    const float scale = muted ? 0.0f : float(envelope) * (9.0f / 8.0f) / (512.0f * 128.0f);
                    MixEnhanced(i, c, wave, {float(c.right) * scale, float(c.left) * scale}, starting);
                }

                MixCamelot(c, wave, muted ? 0 : right | left << 16, meter);
                if (!c.status)
                {
                    EndNote(i, true);
                }

                continue;
            }
        }

        // The master volume scales the envelope's level, and the level scales each side's volume. The mono mixer plays
        // the channel at the average of its volumes. Camelot's channels were marked above.
        const int level = (envelope * (info_.master_volume + 1)) >> 4;
        const int right = info_.mono ? ((c.right + c.left) * level) >> 9 : (c.right * level) >> 8;
        const int left = info_.mono ? right : (c.left * level) >> 8;
        if (!info_.camelot_mixer && (right != 0 || left != 0))
        {
            MarkSounded(i);
        }

        if (enhanced_)
        {
            // The enhanced mode's volumes: the master volume's share of the envelope, times each side's volume, over
            // 2^20. The mono mixer's one side takes the average of the two.
            const float scale = muted ? 0.0f : float(envelope * (info_.master_volume + 1)) / 1048576.0f;
            const std::array<float, 2> volumes =
                info_.mono ? std::array<float, 2>{float(c.right + c.left) * scale * 0.5f,
                                                  float(c.right + c.left) * scale * 0.5f}
                           : std::array<float, 2>{float(c.right) * scale, float(c.left) * scale};
            MixEnhanced(i, c, wave, volumes, starting);
        }

        MixChannel(c, wave, muted ? 0 : right, muted ? 0 : left, meter);
        if (!c.status)
        {
            EndNote(i, true);
        }
    }

    if (mixing_ && info_.camelot_mixer)
    {
        OutputCamelot();
    }

    if (enhanced_)
    {
        EndEnhancedPart();
    }

    // The mono mixer's left side is its right side.
    peaks_ = {};
    if (metering)
    {
        const size_t points = size_t(info_.samples_per_frame);
        for (size_t t = 0; t < tracks_.size(); t++)
        {
            for (size_t side = 0; side < 2; side++)
            {
                const int16_t* sums = &sums_[(2 * t + (info_.mono ? 0 : side)) * points];
                for (size_t i = 0; i < points; i++)
                {
                    peaks_[t][1 - side] = std::max<int32_t>(peaks_[t][1 - side], std::abs(sums[i]));
                }
            }
        }
    }
}

bool Sequencer::MixerSilent() const
{
    const auto silent = [](std::span<const int8_t> points)
    {
        return std::all_of(points.begin(), points.end(), [](int8_t point) { return point == 0; });
    };

    if (!mixing_)
    {
        return true;
    }

    // Camelot's echo comes from the buffer's earlier output, and from the next frame's sums that the output stage
    // starts. With one point for each of the mixer's, the stage starts each half of a silent sum at 64 less, and its
    // rounding gives the 64 back.
    if (info_.camelot_mixer)
    {
        const uint32_t start = info_.camelot_output_points == 1 ? 0xFFBFFFC0 : 0;
        return silent(pcm_) && std::all_of(camelot_sums_.begin(), camelot_sums_.end(),
                                           [start](uint32_t s) { return s == 0 || s == start; });
    }

    return reverb_ ? silent(pcm_) : silent(Right()) && silent(Left());
}

void Sequencer::StartPart()
{
    // The frame's part follows the parts that the DMA has played since it last started the buffer: the buffer's start
    // when the DMA starts it again in the next frame. Camelot's SoundMain works out the place in the same way, from the
    // copy of the DMA counter that the game's VBlank interrupt makes (0x08000682 in Golden Sun: The Lost Age).
    const uint32_t points = uint32_t(info_.samples_per_frame);
    part_ = dma_counter_ > 1 ? (uint32_t(dma_period_) - (dma_counter_ - 1u)) * points * uint32_t(PointsPerMixed()) : 0;
    if (info_.camelot_mixer)
    {
        return;
    }

    int8_t* right = &pcm_[part_];
    int8_t* left = right + kPcmBufferSize;
    if (!reverb_)
    {
        std::fill(right, right + points, int8_t(0));
        if (!info_.mono)
        {
            std::fill(left, left + points, int8_t(0));
        }

        return;
    }

    // The reverb adds both sides' points of the part, from the DMA's last pass over the buffer, to those of the next
    // part, or of the buffer's first part after its last. The part's points on both sides take the sum times the
    // reverb, over 512 and rounded down. A result whose bit 7 is set goes up by 1. The mono mixer adds its one side's
    // points, and takes the sum times the reverb over 256.
    const uint32_t next = dma_counter_ == 2 ? 0 : part_ + points;
    if (info_.mono)
    {
        for (uint32_t i = 0; i < points; i++)
        {
            int32_t value = ((right[i] + pcm_[next + i]) * reverb_) >> 8;
            if (value & 0x80)
            {
                value++;
            }

            right[i] = int8_t(value);
        }

        return;
    }

    for (uint32_t i = 0; i < points; i++)
    {
        const int32_t sum = right[i] + left[i] + pcm_[next + i] + pcm_[kPcmBufferSize + next + i];
        int32_t value = (sum * reverb_) >> 9;
        if (value & 0x80)
        {
            value++;
        }

        right[i] = int8_t(value);
        left[i] = int8_t(value);
    }
}

void Sequencer::MixChannel(Channel& c, const Wave& wave, int right, int left, int meter)
{
    const int samples = info_.samples_per_frame;

    // Each point that the channel plays adds to the frame's part on each side, or on the mono mixer's one side. A
    // track's sums for its meter add the points without wrapping round.
    int8_t* out_right = mixing_ ? &pcm_[part_] : nullptr;
    int8_t* out_left = mixing_ && !info_.mono ? &pcm_[kPcmBufferSize + part_] : nullptr;
    int16_t* sums_right = meter >= 0 ? &sums_[size_t(2 * meter * samples)] : nullptr;
    int16_t* sums_left = sums_right ? sums_right + samples : nullptr;
    const auto mix = [&](int i, int32_t value)
    {
        if (out_right)
        {
            AddPoint(out_right[i], value, right);
        }

        if (out_left)
        {
            AddPoint(out_left[i], value, left);
        }

        if (sums_right)
        {
            sums_right[i] = int16_t(sums_right[i] + ((value * right) >> 8));
            sums_left[i] = int16_t(sums_left[i] + ((value * left) >> 8));
        }
    };

    // Camelot's mixer takes a sample that has no points left when it comes to mix it as a synth voice from then on,
    // unless it plays at a fixed pitch. A sample of no length is one from the start.
    if (info_.camelot_mixer && !(c.type & kVoiceFixed) && (c.synth || c.count == 0))
    {
        c.synth = true;
        MixSynth(c);
        return;
    }

    // A newer driver's mixer plays a reversed sample backwards from its end, and counts its way through a compressed
    // sample by the point rather than the address. A compressed voice with an uncompressed sample isn't mixed.
    const bool special = info_.special_samples && (c.type & (kVoiceReverse | kVoiceCompressed));
    const bool reverse = special && (c.type & kVoiceReverse);
    if (special && !(c.status & kStatusSpecial))
    {
        c.status |= kStatusSpecial;
        if (reverse)
        {
            c.position = 2 * c.wave + 32 + wave.size - c.position;
        }

        if (wave.type != 0)
        {
            c.position -= c.wave + 16;
        }
    }

    if (special && !reverse && wave.type == 0)
    {
        return;
    }

    // Otherwise a fixed-pitch sample plays a point for each point of the output. The driver takes a loop of no length
    // as no loop.
    const uint32_t loop_start = special ? wave.loop_start : wave.Data() + wave.loop_start;
    const int32_t loop_length = int32_t(wave.size - wave.loop_start);
    const bool loops = !reverse && (c.status & kStatusLoop) && loop_length > 0;
    if (!special && (c.type & kVoiceFixed))
    {
        for (int i = 0; i < samples; i++)
        {
            mix(i, rom_.S8(c.position));
            c.position++;
            if (--c.count == 0)
            {
                if (!loops)
                {
                    c.status = 0;
                    return;
                }

                c.position = loop_start;
                c.count = loop_length;
            }
        }

        return;
    }

    // Other samples move on by their rate, in 2^-23 points, for each point of the output. A sample that ends stops the
    // channel there, without saving its progress, and a loop goes back by its length as many times as it takes. A
    // reversed sample doesn't loop.
    //
    // Each point of the output is between two of the sample's points, at the fraction. A reversed sample's position is
    // one past its point, and its next point is before that. The fraction keeps the bits above 29 that the steps carry
    // into it, and the product with the difference wraps round at 32 bits.
    const uint32_t step = special && (c.type & kVoiceFixed) ? 0x800000u : c.frequency * info_.step_scale;
    const bool compressed = special && wave.type != 0;
    DecodedBlock block;
    const auto point = [&](uint32_t at)
    {
        return compressed ? CompressedPoint(rom_, c.wave, at, block) : int32_t(rom_.S8(at));
    };

    uint32_t fraction = c.fraction;
    int32_t count = c.count;
    uint32_t position = c.position;
    for (int i = 0; i < samples; i++)
    {
        if (out_right)
        {
            const int32_t s0 = reverse ? point(position - 1) : point(position);
            const int32_t s1 = reverse ? point(position - 2) : point(position + 1);
            mix(i, s0 + (int32_t(fraction * uint32_t(s1 - s0)) >> 23));
        }

        fraction += step;
        const int32_t advance = int32_t(fraction >> 23);
        if (advance == 0)
        {
            continue;
        }

        fraction &= ~0x3F800000u;
        count -= advance;
        if (count > 0)
        {
            position = reverse ? position - uint32_t(advance) : position + uint32_t(advance);
            continue;
        }

        if (!loops)
        {
            c.status = 0;
            return;
        }

        int32_t overshoot = -count;
        count += loop_length;
        while (count <= 0)
        {
            overshoot -= loop_length;
            count += loop_length;
        }

        position = loop_start + uint32_t(overshoot);
    }

    c.fraction = fraction;
    c.count = count;
    c.position = position;
}

void Sequencer::MixSynth(Channel& c) const
{
    // A synth voice's data follows its sample's 16-byte header. It starts 0x80 and its type: 0 for a pulse wave, 1 for
    // a saw wave, and anything else for a triangle wave.
    const uint32_t data = c.wave + 16;
    const int type = int8_t(rom_.U8(data + 1));
    const uint32_t step = SynthStep(c.frequency, info_.step_scale);
    const int samples = info_.samples_per_frame;

    // A pulse wave's duty follows a triangle wave whose phase is the top byte of the count. The count moves on by the
    // fourth byte of the sample's data each frame.
    if (type == 0)
    {
        c.count = int32_t(uint32_t(c.count) + DutyStep(rom_, data));
        c.fraction += step * uint32_t(samples);
        return;
    }

    // A saw wave goes through a filter that adds half its last output to each point. The count keeps the filter.
    if (type == 1)
    {
        int32_t filter = c.count;
        for (int i = 0; i < samples; i++)
        {
            c.fraction += step;
            filter = FilterSaw(SawPoint(c.fraction), filter);
        }

        c.count = filter;
        return;
    }

    c.fraction += step * uint32_t(samples);
}

} // namespace agblib::mp2k
