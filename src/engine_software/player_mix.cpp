// SPDX-License-Identifier: MIT

// The model's envelopes, its volume and pitch, and its mixer: the chain of channels, the mix and the clip table. Each
// routine names its address in B33E.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"
#include "agblib/engine_software/player.h"

namespace agblib::engine_software
{
namespace
{

// The mixer's handlers, as offsets in the mixer's image in IWRAM. A handler of a channel mixes the first channel that
// plays from it on: each handler before kLooping, kOneShot and kSkip passes over one more channel that doesn't play.
// kFinish writes the sample. At its end, a sample that plays once stores the address 12 bytes past the instruction
// that stores it as its channel's handler, as the ARM7TDMI stores the pc: kEnded. kEnded passes on to the next channel.
constexpr uint32_t kFinish = 0x1C;
constexpr uint32_t kLooping = 0x4C;
constexpr uint32_t kOneShot = 0x84;
constexpr uint32_t kEnded = 0xAC;
constexpr uint32_t kSkip = 0xBC;
constexpr uint32_t kHandlerStep = 4;
constexpr uint32_t kMostSkips = 2;

// The clip table's size for up to 2 music channels, the share of it that the default gain gives the slope, and the
// shift of the sum before the table.
constexpr int kSmallTable = 512;
constexpr int kDefaultGainShift = 5;
constexpr int kSumShift = 6;

// The fade-out's level past which a released channel stops.
constexpr uint32_t kFadeLimit = 0xFFFF;

// Returns how many channels a handler passes over before it mixes, where `base` is the handler that passes over none.
uint32_t Skips(uint32_t handler, uint32_t base)
{
    return (base - handler) / kHandlerStep;
}

bool IsHandler(uint32_t handler, uint32_t base)
{
    return handler <= base && base - handler <= kMostSkips * kHandlerStep && (base - handler) % kHandlerStep == 0;
}

} // namespace

// The clip table (0x08001660): 256 entries a music channel, 512 at least. Its middle part rises from -128 to 127 over
// 16 x the size over the gain. The default gain gives a slope of 512 entries. The mixer reads the entry at the sum of
// the channels shifted right by 6, from the table's middle.
void Player::BuildClipTable()
{
    const int size = music_ <= 2 ? kSmallTable : music_ << 8;
    int32_t gain = int32_t(info_.build.gain);
    if (gain == 0)
    {
        gain = size >> kDefaultGainShift;
    }

    const int32_t span = Divide(size << 4, gain);
    const int32_t low = int32_t(uint32_t(size - span) >> 1);
    const int32_t high = low + span;
    clip_span_ = uint32_t(span);
    clip_.assign(size_t(size), 0);
    for (int32_t i = size - 1; i >= 0; i--)
    {
        if (i < low)
        {
            clip_[size_t(i)] = -128;
        }
        else if (i >= high)
        {
            clip_[size_t(i)] = 127;
        }
        else
        {
            clip_[size_t(i)] = int8_t(uint8_t(Divide((i - low) << 8, high - low) - 0x80));
        }
    }
}

uint32_t Player::ClipSpan() const
{
    return clip_span_;
}

// The BIOS's division. It rounds toward 0. A division by 0 isn't followed.
int32_t Player::Divide(int32_t numerator, int32_t denominator)
{
    if (denominator == 0 || (numerator == std::numeric_limits<int32_t>::min() && denominator == -1))
    {
        unmodelled_++;
        return 0;
    }

    return numerator / denominator;
}

// The envelopes and fade-outs (0x08001D5C), on each music channel that plays: a released channel's fade-out drops by
// its instrument's and stops it below 0, then the volume envelope and the pan envelope take a step.
void Player::Envelopes()
{
    for (int i = 0; i < music_; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (c.flags & kChannelOff)
        {
            continue;
        }

        if (rom_.U8(c.instrument + kInstrumentVolumeEnvelope + kEnvelopeCount) != 0)
        {
            if (c.flags & kChannelReleased)
            {
                c.fade -= rom_.U32(c.instrument + kInstrumentFadeOut);
                if (c.fade > kFadeLimit)
                {
                    c.flags = kChannelOff;
                    continue;
                }
            }

            c.flags |= kChannelVolume;
            StepEnvelope(c.flags, c.instrument + kInstrumentVolumeEnvelope, c.volume_envelope);
        }

        if (rom_.U8(c.instrument + kInstrumentPanEnvelope + kEnvelopeCount) != 0)
        {
            c.flags |= kChannelVolume;
            StepEnvelope(c.flags, c.instrument + kInstrumentPanEnvelope, c.pan_envelope);
        }
    }
}

// An envelope's step (0x08000D90). The value is the line from the point before the tick to the point after it, at the
// tick. Then the tick moves on. At the point after it, the envelope holds at the sustain point until the channel is
// released, goes back to the loop's start past the loop's end, or stays at its last point.
void Player::StepEnvelope(uint16_t flags, uint32_t envelope, EnvelopeState& state)
{
    const auto tick_at = [&](uint32_t point)
    {
        return uint32_t(rom_.U16(envelope + kEnvelopePoints + kEnvelopePointSize * point));
    };
    const auto value_at = [&](uint32_t point)
    {
        return uint32_t(rom_.U16(envelope + kEnvelopePoints + kEnvelopePointSize * point + 2));
    };

    uint32_t from = state.from;
    uint32_t to = state.to;
    uint32_t tick = state.tick;
    if (from != to)
    {
        const int32_t rise = int32_t(value_at(to) - value_at(from)) * int32_t(tick - tick_at(from));
        state.value = uint16_t(value_at(from) + uint32_t(Divide(rise, int32_t(tick_at(to) - tick_at(from)))));
    }
    else
    {
        state.value = uint16_t(value_at(to));
    }

    tick++;
    if (tick >= tick_at(to))
    {
        from = to;
        to++;
        const uint8_t sustain = rom_.U8(envelope + kEnvelopeSustain);
        const uint8_t loop_start = rom_.U8(envelope + kEnvelopeLoopStart);
        const uint8_t loop_end = rom_.U8(envelope + kEnvelopeLoopEnd);
        if (sustain != kEnvelopeNone && !(flags & kChannelReleased) && to > sustain)
        {
            to = sustain;
            from = to;
            tick = tick_at(from);
        }
        else if (loop_start != kEnvelopeNone && to > loop_end)
        {
            to = loop_start;
            from = to;
            if (to != loop_end)
            {
                to++;
            }

            tick = tick_at(from);
        }
        else if (to >= rom_.U8(envelope + kEnvelopeCount))
        {
            to--;
            tick--;
        }
    }

    state.from = uint16_t(from);
    state.to = uint16_t(to);
    state.tick = uint16_t(tick);
}

// The volume and pitch (0x080023A4). A step from the pitch table at the note's index, times the rate's factor. A level
// from the volume times the music volume, times the volume envelope, and times the fade-out while released. Mono
// output alone: the pan doesn't count.
void Player::Update(Channel& c)
{
    const uint16_t flags = c.flags;
    if (flags & kChannelPitch)
    {
        const int32_t index = (int32_t(c.relative) + c.note) * kNoteSteps + c.fine_tune + c.slide;
        if (!rom_.Contains(PitchEntry(info_.build, index), 4))
        {
            unmodelled_++;
        }

        c.step = PitchStep(rom_, info_, index);
    }

    if (flags & kChannelVolume)
    {
        const uint32_t envelope = c.volume_envelope.value;
        const int32_t volume = (int32_t(c.volume) * int32_t(music_volume_)) >> 8;
        if (info_.build.stereo)
        {
            unmodelled_++;
        }

        if (flags & kChannelReleased)
        {
            c.level = (uint32_t(volume) * envelope * c.fade) >> 22;
        }
        else
        {
            c.level = (uint32_t(volume) * envelope) >> 6;
        }
    }

    c.flags = uint16_t(flags & ~(kChannelPitch | kChannelVolume));
}

// The vibrato (0x080024CC): the slide becomes the sine at the place times the depth over 32, and the place moves on by
// the speed.
void Player::Vibrato(Channel& c)
{
    const int32_t sine = int16_t(rom_.U16(info_.build.sine + 2 * (uint32_t(c.vibrato_place) & (kSinePoints - 1))));
    c.slide = int16_t((sine * c.vibrato_depth) >> 5);
    c.vibrato_place = uint8_t(c.vibrato_place + c.vibrato_speed);
}

// The tremolo (0x080024F8): the place moves on by the vibrato's speed, and the volume gains the sine at the place
// times the vibrato's depth over 32, within 0 to 64.
void Player::Tremolo(Channel& c)
{
    c.vibrato_place = uint8_t(c.vibrato_place + c.vibrato_speed);
    const int32_t sine = int16_t(rom_.U16(info_.build.sine + 2 * (uint32_t(c.vibrato_place) & (kSinePoints - 1))));
    int8_t volume = int8_t(uint8_t(c.volume) + ((sine * c.vibrato_depth) >> 5));
    if (volume > 0x40)
    {
        volume = 0x40;
    }
    else if (volume < 0)
    {
        volume = 0;
    }

    c.volume = volume;
}

// The chain of channels (0x08000628), from the last channel to the first. A channel that plays gets the handler of its
// sample's kind. One that doesn't passes over to the next channel's handler: kFinish where only channels that don't
// play follow, else a handler that passes over one more, up to two, and then kSkip. The mix starts at the first
// channel that plays.
void Player::BuildChain()
{
    const int total = music_ + effects_;
    channels_[size_t(total)].handler = kFinish;
    uint32_t run = 0;
    uint32_t skips = 0;
    for (int i = total - 1; i >= 0; i--)
    {
        Channel& c = channels_[size_t(i)];
        if (c.flags & (kChannelOff | kChannelPaused))
        {
            const uint32_t next = channels_[size_t(i) + 1].handler;
            if (next == kFinish)
            {
                c.handler = kFinish;
            }
            else if (skips < kMostSkips)
            {
                c.handler = next - kHandlerStep;
                skips++;
            }
            else
            {
                c.handler = kSkip;
                skips = 0;
            }

            run++;
        }
        else
        {
            c.handler = c.loop_bytes != 0 ? kLooping : kOneShot;
            run = 0;
            skips = 0;
        }
    }

    chain_start_ = run;
}

int8_t Player::Point(uint32_t address)
{
    if (!rom_.Contains(address))
    {
        unmodelled_++;
        return 0;
    }

    return rom_.S8(address);
}

// The mix (0x08001844 in the cartridge, copied to 0x03000000) of `count` samples. For each sample, the handlers of the
// chain add each playing channel's point times its level to the sum and step its place. A looping sample goes back by
// its loop's bytes past its end. A sample that plays once stops its channel at its end, and its handler then passes
// over it. The handler of the channel before it may still pass over to it, and then mixes its last point again until
// the next chain. The clip table gives the sample from the sum.
void Player::Mix(uint32_t count)
{
    std::vector<int8_t>& buffer = buffers_[size_t(buffer_)];
    const int32_t middle = int32_t(clip_.size() / 2);
    const size_t last = channels_.size();
    for (uint32_t n = 0; n < count; n++)
    {
        int32_t sum = 0;
        size_t next = chain_start_ + 1;
        uint32_t handler = channels_[chain_start_].handler;
        while (handler != kFinish)
        {
            if (IsHandler(handler, kLooping) || IsHandler(handler, kOneShot))
            {
                const bool loops = IsHandler(handler, kLooping);
                next += Skips(handler, loops ? kLooping : kOneShot);
                if (next > last)
                {
                    unmodelled_++;
                    break;
                }

                Channel& c = channels_[next - 1];
                const uint32_t moved = c.fraction + c.step;
                sum = int32_t(uint32_t(sum) + uint32_t(int32_t(Point(c.place))) * c.level);
                c.fraction = moved & 0xFFFF;
                uint32_t pointer = c.place + (moved >> 16);
                const bool below_end = int32_t(pointer) < int32_t(c.end);
                if (loops)
                {
                    if (!below_end)
                    {
                        pointer -= c.loop_bytes;
                    }

                    c.place = pointer;
                }
                else if (below_end)
                {
                    c.place = pointer;
                }
                else
                {
                    c.handler = kEnded;
                    c.flags = kChannelOff;
                    c.delay = 0;
                    c.retrigger = 0;
                }
            }
            else if (IsHandler(handler, kSkip))
            {
                next += Skips(handler, kSkip);
            }
            else if (handler != kEnded)
            {
                unmodelled_++;
                break;
            }

            if (next >= last)
            {
                unmodelled_++;
                break;
            }

            handler = channels_[next].handler;
            next++;
        }

        int32_t index = middle + (sum >> kSumShift);
        if (index < 0 || index >= int32_t(clip_.size()))
        {
            unmodelled_++;
            index = std::clamp<int32_t>(index, 0, int32_t(clip_.size()) - 1);
        }

        if (written_ < buffer.size())
        {
            buffer[written_] = clip_[size_t(index)];
        }

        written_++;
    }

    elapsed_ += count;
}

} // namespace agblib::engine_software
