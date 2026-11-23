// SPDX-License-Identifier: MIT

#include "agblib/logikstate/mod30_player.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/logikstate/mod30.h"
#include "mod30_player_internal.h"

namespace agblib::logikstate
{
Mod30Player::Mod30Player(const Rom& rom, uint32_t module, const Mod30Settings& settings)
    : rom_(rom), settings_(settings), build_(settings_.build)
{
    if (!ReadMod30Module(rom, module, module_) || !ReadMod30Bank(rom, settings.bank, settings.header_size, bank_) ||
        settings.rate_index < 0 || settings.rate_index >= int(kMod30Rates.size()))
    {
        warnings_.push_back("the module at " + agb::Hex(module) + " can't be read");
        return;
    }

    // The init: the rate and the volumes. The play routine of the first builds loads and starts the module at once.
    rate_ = kMod30Rates[size_t(settings.rate_index)];
    frame_samples_ = kMod30FrameSamples[size_t(settings.rate_index)];
    valid_ = true;
    channels_ = LoadedChannels();
    if (!build_.play_request)
    {
        Start();
    }
}

bool Mod30Player::Valid() const
{
    return valid_;
}

const Mod30Module& Mod30Player::Header() const
{
    return module_;
}

const Mod30Bank& Mod30Player::Bank() const
{
    return bank_;
}

void Mod30Player::OnStep(std::function<void(bool)> callback)
{
    on_step_ = std::move(callback);
}

uint64_t Mod30Player::Elapsed() const
{
    return elapsed_;
}

bool Mod30Player::Playing() const
{
    return valid_ && (playing_ != 0 || !started_);
}

int Mod30Player::RowOrder() const
{
    return row_order_;
}

int Mod30Player::RowNumber() const
{
    return row_number_;
}

uint32_t Mod30Player::Speed() const
{
    return speed_;
}

uint32_t Mod30Player::Tempo() const
{
    return tempo_;
}

uint32_t Mod30Player::RowSamples() const
{
    return uint32_t(row_samples_);
}

uint32_t Mod30Player::TickSamples() const
{
    return uint32_t(tick_samples_);
}

uint32_t Mod30Player::Rate() const
{
    return rate_;
}

int Mod30Player::Channels() const
{
    return channels_;
}

const std::array<Mod30Channel, kMod30Records>& Mod30Player::Records() const
{
    return records_;
}

int32_t Mod30Player::MixVolume(const Mod30Channel& channel) const
{
    int32_t v = (channel.volume * settings_.music) >> 6;
    v = (v * settings_.master) >> 6;
    v = (v * int32_t(fade_)) >> 6;

    return std::max(v, 0);
}

bool Mod30Player::Sounding(const Mod30Channel& channel, int number) const
{
    const int mixed =
        build_.mix_cap != 0 && settings_.mixer_mode == 0 ? std::min(build_.mix_cap, channels_) : channels_;
    if (number >= mixed || channel.points == 0 || channel.length <= kStoppedLength)
    {
        return false;
    }

    return !(settings_.mixer_mode == 0 && build_.scaled_zero_waits && MixVolume(channel) == 0);
}

double Mod30Player::RootRate(int sample) const
{
    Mod30Channel ch;
    ch.relative_note = bank_.samples[size_t(sample)].relative_note;
    ch.fine_tune = bank_.samples[size_t(sample)].fine_tune;
    if (module_.amiga != 0)
    {
        const int32_t period = Period(uint32_t(ch.fine_tune + kMod30RootNote));
        return period > 0 ? double(kPeriodClock) / (2.0 * period) : 0;
    }

    // The frequency that the driver works the step out from, before it divides by the mixer's rate.
    const int32_t y = kLinearTop - int32_t(LinearPeriod(ch, kMod30RootNote));
    const int32_t octaves = Divide(y, kOctave);
    const int32_t place = std::clamp(Remainder(y, kOctave), 0, kOctave - 1);
    const double entry = rom_.U16(settings_.linear + uint32_t(place * 2)) * 4.0;

    return std::ldexp(entry, octaves - 7);
}

std::vector<uint32_t> Mod30Player::RepeatState() const
{
    std::vector<uint32_t> state = {speed_, tempo_};
    for (int c = 0; c < channels_; c++)
    {
        const Mod30Channel& ch = records_[size_t(c)];
        state.push_back(uint32_t(ch.loop_row) << 16 | ch.loop_count);
        for (const Stream& s : decoders_[size_t(c)].streams)
        {
            state.push_back(s.address);
            state.push_back(uint32_t(s.count));
        }
    }

    return state;
}

const std::vector<std::string>& Mod30Player::Warnings() const
{
    return warnings_;
}

int Mod30Player::LoadedChannels() const
{
    if (build_.channel_cap != 0 && module_.channels > build_.channel_cap)
    {
        return int(build_.channel_cap);
    }

    return int(module_.channels);
}

void Mod30Player::Start()
{
    // The load: the module's speed, tempo and channels, the latter up to the build's cap. The sound effects take the
    // records after the module's channels.
    speed_ = module_.speed;
    tempo_ = module_.tempo;
    channels_ = LoadedChannels();
    const int cap = build_.channel_cap != 0 ? int(build_.channel_cap) : build_.records;
    end_channel_ = channels_ + std::min(cap - int(module_.channels), settings_.sfx_channels);

    // The start at order 0.
    fade_ = kFullFade;
    hz_ = uint32_t(Divide(int32_t((tempo_ * 50) << 16), 125) >> 16);
    row_samples_ = Divide(int32_t(rate_ * speed_), int32_t(hz_));
    tick_samples_ = Divide(int32_t(rate_), int32_t(hz_));
    order_ = 0;
    row_ = 0;
    row_left_ = 0;
    tick_left_ = 0;
    for (int c = 0; c < build_.records; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        if (build_.start_clears_records)
        {
            ch = Mod30Channel();
        }
        else
        {
            ch.volume = 0;
            ch.length = 0;
            ch.loop_length = 0;
        }
    }

    broke_ = 0;
    tick_ = 0;
    playing_ = 1;
    started_ = true;
}

void Mod30Player::Frame()
{
    if (!valid_)
    {
        return;
    }

    // The fade rises each frame. A play request then starts its module.
    fade_ = std::min(fade_ + kFadeStep, kFullFade);
    if (!started_)
    {
        Start();
    }

    MixFrame();
    frames_++;
    elapsed_ = std::max(elapsed_, frames_ * frame_samples_);
}

void Mod30Player::MixFrame()
{
    const int32_t size = int32_t(frame_samples_);
    const bool full = build_.frame_loop == Mod30FrameLoop::kFull;
    const bool mode1 = settings_.mixer_mode == 1;
    if (build_.frame_loop == Mod30FrameLoop::kShort && playing_ == 0)
    {
        return;
    }

    // The first builds count each pass as one sample more than it mixes, and the frame can end a few samples short.
    int32_t done = 0;
    int32_t left = size;
    for (int pass = 0; pass < kMaxPasses; pass++)
    {
        if (full ? done >= size : left <= 0)
        {
            return;
        }

        int32_t n = size;
        if (playing_ != 0 || build_.frame_loop == Mod30FrameLoop::kShort)
        {
            n = RowAndTick(done);
        }

        // The mix of the pass. Mixer mode 1 ends a pass early where a channel's sample ends.
        int listed = 0;
        if (mode1)
        {
            n = ListChannels(end_channel_, n, listed);
        }
        else
        {
            const int count = build_.mix_cap != 0 ? std::min(build_.mix_cap, channels_) : channels_;
            MixChannels(0, count, uint32_t(std::max(n, 0)));
        }

        if (full)
        {
            if (done + n > size)
            {
                n = size - done;
            }

            if (playing_ != 0)
            {
                n = std::min({n, row_left_, tick_left_});
            }
        }

        if (mode1 && listed != 0)
        {
            AdvanceListed(end_channel_, n);
        }

        done += n;
        row_left_ -= n;
        tick_left_ -= n;
        left -= n + (full ? 0 : 1);
        if (n <= 0 && full)
        {
            Warn("a pass of the mixer has no samples, which the driver can't play");
            valid_ = false;
            return;
        }
    }

    Warn("a frame doesn't end, which the driver can't play");
    valid_ = false;
}

int32_t Mod30Player::RowAndTick(int32_t done)
{
    const int32_t size = int32_t(frame_samples_);
    elapsed_ = frames_ * frame_samples_ + uint64_t(done);
    if (row_left_ == 0)
    {
        Row();
        row_left_ = row_samples_;
        if (on_step_)
        {
            on_step_(true);
        }
    }

    if (tick_left_ == 0)
    {
        if (build_.tick_with_row || row_left_ != row_samples_)
        {
            Tick();
            if (on_step_)
            {
                on_step_(false);
            }
        }

        tick_left_ = tick_samples_;
    }

    int32_t n = std::min(row_left_, tick_left_);
    if (build_.frame_loop != Mod30FrameLoop::kFull && n > size)
    {
        n = size;
    }

    if (done + n > size)
    {
        n = size - done;
    }

    return n;
}

void Mod30Player::MixChannels(int first, int end, uint32_t samples)
{
    for (int c = first; c < end; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        if (ch.points == 0 || ch.length <= kStoppedLength)
        {
            continue;
        }
        if (build_.scaled_zero_waits && MixVolume(ch) == 0)
        {
            continue;
        }

        Advance(ch, samples);
    }
}

int32_t Mod30Player::ListChannels(int end, int32_t samples, int& listed)
{
    // A pass ends where a channel's sample ends. Group C' lists every channel, and ends the pass at the sample's end
    // itself. The later builds end it a point before.
    const bool listed_all = build_.frame_loop == Mod30FrameLoop::kShortList;
    for (int c = 0; c < end; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        if (ch.points == 0 && !listed_all)
        {
            continue;
        }

        listed++;
        if (build_.mode1_volume_zero && ch.length <= 1)
        {
            ch.volume = 0;
        }

        if (ch.length != 0 && ch.volume != 0)
        {
            const uint32_t e = ch.position + ch.step * uint32_t(samples);
            const uint32_t limit = listed_all ? ch.length : ch.length - 0x1000;
            if (e > limit)
            {
                samples -= Divide(int32_t(e - ch.length), int32_t(ch.step));
            }
        }
    }

    return samples;
}

void Mod30Player::AdvanceListed(int end, int32_t samples)
{
    const bool listed_all = build_.frame_loop == Mod30FrameLoop::kShortList;
    for (int c = 0; c < end; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        if (!listed_all && ch.length == 0 && ch.volume == 0)
        {
            continue;
        }

        ch.position += ch.step * uint32_t(samples);
        if (ch.position >= ch.length)
        {
            ch.points += ch.loop_start;
            ch.position -= ch.length;
            ch.length = ch.loop_length;
            ch.loop_start = 0;
            ch.looped = true;
        }
    }
}

void Mod30Player::Advance(Mod30Channel& ch, uint32_t samples)
{
    // Each sample: at the end, the points move on to the loop, the loop becomes the length, and the place keeps its
    // overshoot. A sample without a loop stops there. Then the mixer reads a point and steps.
    while (samples > 0 && ch.length != 0)
    {
        if (int32_t(ch.position) >= int32_t(ch.length))
        {
            ch.points += ch.loop_start;
            ch.loop_start = 0;
            ch.position -= ch.length;
            ch.length = ch.loop_length;
            ch.looped = true;
            if (ch.length == 0)
            {
                return;
            }

            ch.position += ch.step;
            samples--;
            continue;
        }

        const int64_t room = int64_t(int32_t(ch.length)) - int32_t(ch.position);
        const uint64_t until = ch.step ? uint64_t((room + ch.step - 1) / ch.step) : samples;
        const uint32_t run = uint32_t(std::min<uint64_t>(samples, until));
        ch.position += run * ch.step;
        samples -= run;
    }
}

void Mod30Player::Row()
{
    if (playing_ == 0)
    {
        return;
    }

    tick_ = 0;
    arpeggio_tick_ = 0;
    broke_ = 0;
    row_order_ = int(order_);
    row_number_ = int(row_);
    uint32_t block = 0;
    for (int c = 0; c < channels_; c++)
    {
        Decode(c, block);
    }

    row_++;
    for (int c = 0; c < channels_; c++)
    {
        PlayCell(records_[size_t(c)], c);
    }

    // The order's end, after its last row or a jump.
    if (row_ == module_.rows)
    {
        order_++;
        if (order_ >= module_.order_count)
        {
            order_ = 0;
            if (!build_.loop_flag || !settings_.loop)
            {
                playing_ = 0;
            }
        }

        row_ = 0;
    }
}

uint32_t Mod30Player::PatternBlock(uint32_t order) const
{
    const Mod30HeaderLayout& layout = HeaderLayout(module_.layout);
    const uint32_t pattern = rom_.U8(module_.address + layout.orders + order);
    if (module_.pattern_offsets)
    {
        return module_.address + layout.patterns + rom_.U32(module_.address + layout.pattern_table + 4 * pattern);
    }

    return module_.patterns[pattern];
}

uint32_t Mod30Player::RepeatOrders() const
{
    const uint32_t count = module_.order_count;
    if (!build_.loop_flag || !settings_.loop)
    {
        return count;
    }

    // The shortest period of the orders' first blocks, counted round from the last order to the first.
    for (uint32_t period = 1; period < count; period++)
    {
        if (count % period != 0)
        {
            continue;
        }

        bool repeats = true;
        for (uint32_t o = period; o < count && repeats; o++)
        {
            repeats = PatternBlock(o) == PatternBlock(o - period);
        }

        if (repeats)
        {
            return period;
        }
    }

    return count;
}

} // namespace agblib::logikstate
