// SPDX-License-Identifier: MIT

#include "agblib/frontline_mod/player.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/frontline_mod/driver.h"

namespace agblib::frontline_mod
{
namespace
{

// The bias of each sum, and the clip table's entries.
constexpr uint32_t kBias = 0x400;
constexpr uint32_t kClipEntries = 2048;

// The music's volume and a channel's volume at their fullest.
constexpr int32_t kMaxMusicVolume = 256;
constexpr int32_t kMaxVolume = 64;

// The phases of the vibrato's table.
constexpr uint32_t kVibratoPhases = 64;

} // namespace

Player::Player(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
    const uint32_t vibrato = info_.ImageAddress(info_.vibrato);
    for (uint32_t i = 0; i < kVibratoPhases; i++)
    {
        vibrato_[i] = rom_.S8(vibrato + i);
    }

    // The init's builder of the clip table: (sum - low) x 256 / span - 128 for the sums from low to low + span, and the
    // extremes outside them.
    const int32_t span = int32_t(info_.mixer_settings.ClipSpan());
    const int32_t low = (int32_t(kClipEntries) - span) / 2;
    const int32_t high = low + span;
    for (int32_t i = 0; i < int32_t(kClipEntries); i++)
    {
        if (i < low)
        {
            clip_[i] = -128;
        }
        else if (i < high)
        {
            clip_[i] = int8_t((i - low) * 256 / span - 128);
        }
        else
        {
            clip_[i] = 127;
        }
    }

    channels_.resize(info_.mixer_settings.channels);
    sums_.resize(info_.mixer_settings.samples / 2);
    output_.resize(info_.mixer_settings.samples);
}

void Player::Start(const Module& module, uint32_t order, int32_t step)
{
    on_ = 0;
    header_ = 0;
    if (rom_.U32(module.address + kHeaderOrderCount) == 0)
    {
        return;
    }

    pattern_ = rom_.U32(module.orders + 4 * order);
    if (rom_.U8(pattern_ + kPatternRows) == 0)
    {
        return;
    }

    order_ = order;
    row_ = 0;
    data_ = rom_.U32(pattern_ + kPatternCells);
    speed_ = int32_t(rom_.U32(module.address + kHeaderSpeed));
    tick_ = speed_;
    on_ = 2;
    header_ = module.address;

    // The game's start stores the step only while the music's volume is 0 and the pause flag (the state's +0x24) is
    // clear. The model has no pause flag: no game starts a song while the music is paused.
    if (music_volume_ == 0)
    {
        fade_step_ = step;
    }
}

void Player::Frame()
{
    Play();
    Mix();
}

bool Player::Play()
{
    if (on_ == 0)
    {
        return false;
    }

    const int32_t tick = tick_ + 1;
    if (tick < speed_)
    {
        tick_ = tick;
        for (uint32_t c = 0; c < MusicChannels(); c++)
        {
            if (channels_[c].tick != Tick::kNone)
            {
                RunTick(channels_[c]);
            }
        }

        return false;
    }

    tick_ = 0;
    ReadRow();
    return true;
}

void Player::ReadRow()
{
    uint32_t at = data_;
    bool jumped = false;
    for (uint32_t i = 0; i < MusicChannels(); i++)
    {
        // Each cell sets the channel's routine of the ticks between rows, and an empty cell clears it.
        Channel& c = channels_[i];
        const uint8_t flags = rom_.U8(at++);
        c.tick = Tick::kNone;
        if (flags == 0)
        {
            continue;
        }

        const bool note = flags & kCellNote;
        if (note)
        {
            at = ReadNote(c, flags, at);
        }

        if (flags & kCellVolume)
        {
            c.volume = rom_.U8(at++);
        }

        const uint8_t effect = flags & kCellEffect;
        if (effect != 0)
        {
            const uint32_t parameter = rom_.U8(at++);
            RunEffect(c, effect, parameter, note, jumped);
        }
    }

    // A jump moves the player to its order's first row, and the row ends there.
    if (jumped)
    {
        return;
    }

    data_ = at;
    row_++;
    if (int32_t(row_) >= int32_t(rom_.U32(pattern_ + kPatternRows)))
    {
        row_ = 0;
        order_++;
        if (int32_t(order_) >= int32_t(rom_.U32(header_ + kHeaderOrderCount)))
        {
            order_ = 0;
        }

        MoveTo(order_);
    }

    // The fade, then each channel's level.
    int32_t volume = music_volume_;
    if (fade_step_ != 0)
    {
        volume += fade_step_;
    }

    if (volume < 0)
    {
        volume = 0;
        fade_step_ = 0;
    }

    if (volume > kMaxMusicVolume)
    {
        volume = kMaxMusicVolume;
        fade_step_ = 0;
    }

    music_volume_ = volume;
    for (uint32_t i = 0; i < MusicChannels(); i++)
    {
        channels_[i].level = (channels_[i].volume * uint32_t(volume)) >> 8;
    }
}

void Player::MoveTo(uint32_t order)
{
    pattern_ = rom_.U32(rom_.U32(header_ + kHeaderOrders) + 4 * order);
    data_ = rom_.U32(pattern_ + kPatternCells);
    order_ = order;
}

uint32_t Player::ReadNote(Channel& c, uint8_t flags, uint32_t at)
{
    const uint32_t index = rom_.U16(at);
    at += 2;
    c.target = index;

    // A cell with the flags 0x91, a slide's target with a volume, takes a path of the game's code that the model
    // doesn't follow: the model reads it as a note. No song of the three games has such a cell.
    if ((flags & 0xFE) == (kCellSlideTarget & 0xFE))
    {
        return at + 1;
    }

    SetStep(c, int32_t(index));
    c.period = int32_t(index);
    const uint32_t number = rom_.U8(at++);
    uint32_t instrument = number;
    if (instrument == 0)
    {
        instrument = c.memory;
    }
    else
    {
        c.memory = instrument;
    }

    const uint32_t record = rom_.U32(rom_.U32(header_ + kHeaderInstruments) + 4 * instrument);
    if (!rom_.Contains(record, 16))
    {
        unmodelled_++;
    }

    if (number != 0)
    {
        c.volume = rom_.U32(record + kInstrumentVolume);
    }

    const uint32_t sample = rom_.U32(record + kInstrumentSample);
    const uint32_t points = rom_.U32(sample + kSamplePoints);
    c.position = points;
    c.end = points + rom_.U32(sample + kSampleLength);
    c.loop_start = points + rom_.U32(record + kInstrumentLoopStart);
    const uint32_t loop_end = rom_.U32(record + kInstrumentLoopEnd);
    c.loop_end = loop_end != 0 ? points + loop_end : 0;
    c.fraction = 0;
    c.on = 2;
    c.instrument = instrument;
    c.starts++;
    c.wraps = 0;
    return at;
}

void Player::RunEffect(Channel& c, int effect, uint32_t parameter, bool note, bool& jumped)
{
    // The cell gives the handler's byte offset in the table. An offset that isn't a word's reads a word turned around.
    // No handler starts at such a word.
    if (effect % 4 != 0)
    {
        unmodelled_++;
        return;
    }

    switch (effect / 4)
    {
    case kEffectParameter:
        c.parameter = parameter;
        break;

    case kEffectSlideDown:
    case kEffectSlideUp:
    case kEffectSlideToNote:
        if (parameter != 0)
        {
            c.parameter = parameter;
        }

        c.tick = effect / 4 == kEffectSlideDown ? Tick::kSlideDown
                 : effect / 4 == kEffectSlideUp ? Tick::kSlideUp
                                                : Tick::kSlideToNote;
        break;

    case kEffectVolume:
        c.volume = parameter;
        break;

    case kEffectVolumeSlide:
        c.parameter = parameter;
        c.tick = Tick::kVolumeSlide;
        break;

    case kEffectVibrato:
    case kEffectVibrato2:
        if (note)
        {
            c.vibrato_phase = 0;
        }

        if (parameter != 0)
        {
            c.parameter = parameter;
            c.vibrato_parameter = parameter;
        }

        c.tick = Tick::kVibrato;
        break;

    case kEffectSpeed:
        speed_ = int32_t(parameter);
        break;

    case kEffectVolumeVibrato:
        c.parameter = parameter;
        c.tick = Tick::kVolumeVibrato;
        break;

    case kEffectJump:
        MoveTo(parameter);
        row_ = 0;
        jumped = true;
        break;

    case kEffectSampleOffset:
        if (parameter != 0)
        {
            c.memory = parameter;
        }
        else
        {
            parameter = c.memory;
        }

        c.position += parameter << 8;
        c.starts++;
        c.wraps = 0;
        break;

    case kEffects - 1:
        // The table's last entry is 0: the player would jump to the BIOS.
        unmodelled_++;
        break;

    default:
        break;
    }
}

void Player::RunTick(Channel& c)
{
    const uint32_t p = c.parameter;
    switch (c.tick)
    {
    case Tick::kSlideDown:
        c.period -= int32_t(p * 4);
        SetStep(c, c.period);
        break;

    case Tick::kSlideUp:
        c.period += int32_t(p * 4);
        SetStep(c, c.period);
        break;

    case Tick::kSlideToNote:
        {
            // A slide up moves by the step and stops at the target. A slide down reaches the target at once: both of
            // the handler's paths down end on it.
            const int32_t target = int32_t(c.target);
            if (c.period == target)
            {
                break;
            }

            int32_t period = target;
            if (c.period < target && c.period + int32_t(p * 4) < target)
            {
                period = c.period + int32_t(p * 4);
            }

            c.period = period;
            SetStep(c, period);
            break;
        }

    case Tick::kVolumeSlide:
    case Tick::kVolumeVibrato:
        {
            int32_t volume = int32_t(c.volume) - int32_t(p & 15);
            if (volume < 0)
            {
                volume = 0;
            }

            volume += int32_t(p >> 4);
            c.volume = uint32_t(volume) > uint32_t(kMaxVolume) ? uint32_t(kMaxVolume) : uint32_t(volume);
            if (c.tick == Tick::kVolumeVibrato && c.vibrato_parameter != 0)
            {
                Vibrato(c, c.vibrato_parameter);
            }
            break;
        }

    case Tick::kVibrato:
        Vibrato(c, p);
        break;

    case Tick::kNone:
        break;
    }
}

void Player::Vibrato(Channel& c, uint32_t parameter)
{
    uint32_t phase = c.vibrato_phase + (parameter >> 4);
    if (phase >= kVibratoPhases)
    {
        phase -= kVibratoPhases;
    }

    c.vibrato_phase = phase;

    // The model follows the game's code only where the vibrato stays within the table of periods. No song of the three
    // games takes it past the table's end.
    const int32_t offset = (int32_t(vibrato_[phase]) * int32_t(parameter & 15)) >> 7;
    const uint32_t index = uint32_t(c.period) + uint32_t(offset) * 4;
    c.step = rom_.U32(info_.periods + index * 4);
}

// The model follows the game's code only for indexes within the table of periods. No song of the three games slides
// below index 0.
void Player::SetStep(Channel& c, int32_t index)
{
    c.step = rom_.U32(info_.periods + uint32_t(index) * 4);
}

void Player::Mix()
{
    std::fill(sums_.begin(), sums_.end(), kBias | (kBias << 16));
    for (Channel& c : channels_)
    {
        if (c.on == 0)
        {
            continue;
        }

        const uint32_t end = c.loop_end != 0 ? c.loop_end : c.end;
        const uint32_t gain = (c.level * info_.mixer_settings.master) >> 6;
        uint32_t fraction = c.fraction;
        uint32_t position = c.position;
        bool stopped = false;
        for (uint32_t& sum : sums_)
        {
            // The end is checked once every two points.
            uint32_t at = position + (fraction >> 16);
            if (int32_t(at) > int32_t(end))
            {
                if (c.loop_end == 0)
                {
                    c.on = 0;
                    position = 0;
                    stopped = true;
                    break;
                }

                fraction = 0;
                position = c.loop_start;
                at = position;
                c.wraps++;
            }

            const uint32_t first = uint32_t(int32_t(uint32_t(int32_t(rom_.S8(at))) * gain) >> 6) & 0xFFFF;
            fraction += c.step;
            at = position + (fraction >> 16);
            const uint32_t second = uint32_t(int32_t(uint32_t(int32_t(rom_.S8(at))) * gain) >> 6) & 0xFFFF;
            fraction += c.step;
            sum += first | (second << 16);
        }

        if (!stopped)
        {
            position += fraction >> 16;
        }

        c.position = position;
        c.fraction = fraction & 0xFFFF;
    }

    // Each sum, less the bias, through the clip table.
    for (size_t i = 0; i < sums_.size(); i++)
    {
        const uint32_t sums[2] = {sums_[i] & 0xFFFF, sums_[i] >> 16};
        for (int k = 0; k < 2; k++)
        {
            if (sums[k] >= kClipEntries)
            {
                unmodelled_++;
                output_[2 * i + k] = 0;
            }
            else
            {
                output_[2 * i + k] = clip_[sums[k]];
            }
        }
    }
}

const std::vector<int8_t>& Player::Output() const
{
    return output_;
}

bool Player::Playing() const
{
    return on_ != 0;
}

uint32_t Player::Order() const
{
    return order_;
}

uint32_t Player::Row() const
{
    return row_;
}

uint32_t Player::Speed() const
{
    return uint32_t(speed_);
}

int32_t Player::MusicVolume() const
{
    return music_volume_;
}

int32_t Player::FadeStep() const
{
    return fade_step_;
}

const Channel& Player::ChannelAt(uint32_t channel) const
{
    return channels_[channel];
}

uint32_t Player::MusicChannels() const
{
    return header_ != 0 ? std::min<uint32_t>(rom_.U32(header_ + kHeaderChannels), uint32_t(channels_.size())) : 0;
}

uint64_t Player::Unmodelled() const
{
    return unmodelled_;
}

} // namespace agblib::frontline_mod
