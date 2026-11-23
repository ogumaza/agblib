// SPDX-License-Identifier: MIT

// The MOD-30 player's cells and effects, and its pitches.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "agblib/agb/rom.h"
#include "agblib/logikstate/mod30.h"
#include "agblib/logikstate/mod30_player.h"
#include "mod30_player_internal.h"

namespace agblib::logikstate
{
namespace
{

// Shifts as ARM's register-specified LSR does: by the low byte of `amount`, and to 0 from 32 on.
uint32_t ShiftRight(uint32_t value, uint32_t amount)
{
    amount &= 0xFF;
    return amount >= 32 ? 0 : value >> amount;
}

} // namespace

void Mod30Player::Decode(int c, uint32_t& block)
{
    Decoder& d = decoders_[size_t(c)];
    const auto next = [this](Stream& s, bool wide)
    {
        if (s.count <= 0)
        {
            s.count = rom_.U8(s.address);
            s.value = wide ? uint32_t(rom_.U8(s.address + 1) << 8 | rom_.U8(s.address + 2)) : rom_.U8(s.address + 1);
            s.address += wide ? 3 : 2;
        }

        s.count--;

        return s.value;
    };

    // Row 0 starts each channel's streams in its block of the order's pattern.
    if (row_ == 0)
    {
        if (block == 0)
        {
            block = PatternBlock(order_);
        }

        const uint32_t data = block + kMod30BlockHeader;
        d.streams[0] = {data, 0, 0};
        d.streams[1] = {data + rom_.U32(block + kMod30BlockStreamB), 0, 0};
        d.streams[2] = {data + rom_.U32(block + kMod30BlockStreamC), 0, 0};
        block = data + rom_.U32(block + kMod30BlockSize);
    }

    const uint32_t a = next(d.streams[0], true);
    d.note = uint16_t(a >> kMod30NoteShift);
    d.sample = uint8_t(a & kMod30CellSample);
    d.volume = uint8_t(next(d.streams[1], false));
    const uint32_t e = next(d.streams[2], true);
    d.effect = uint8_t(e >> 8);
    d.operand = uint8_t(e);
}

void Mod30Player::PlayCell(Mod30Channel& ch, int c)
{
    const Decoder& d = decoders_[size_t(c)];
    uint16_t note = d.note;
    uint8_t sample = d.sample;
    const uint8_t effect = d.effect;
    ch.volume_column = d.volume;

    // A slide to a note keeps the note as its target, and takes the volume of the cell's sample.
    if ((effect & 0xF) == kSlideToNote)
    {
        if (note != kMod30NoNote)
        {
            ch.target = int16_t(note);
        }

        note = kMod30NoNote;
        if (sample != 0)
        {
            ch.volume = bank_.samples[sample - 1u].volume;
        }

        sample = 0;
    }

    ch.effect = -1;
    ch.vibrato = 0;
    if (note != kMod30NoNote)
    {
        // A note starts the channel's sample, or the cell's, from its first point.
        if (sample != 0)
        {
            ch.sample = uint16_t(sample - 1);
            ch.volume = bank_.samples[ch.sample].volume;
        }

        const Mod30Sample& h = bank_.samples[ch.sample];
        const auto scaled = [this](uint32_t points)
        {
            return build_.short_lengths && points != 0 ? (points << kFractionBits) - 1 : points << kFractionBits;
        };

        ch.points = bank_.points[ch.sample];
        ch.length = scaled(h.length);
        ch.loop_start = h.loop_start;
        ch.loop_length = scaled(h.loop_length);
        ch.fine_tune = h.fine_tune;
        ch.relative_note = h.relative_note;
        ch.note = note;
        ch.position = 0;
        ch.packed = 0;
        ch.starts++;
        ch.first_point = ch.points;
        ch.whole_length = h.length;
        ch.whole_loop_start = h.loop_start;
        ch.whole_loop_length = h.loop_length;
        ch.looped = false;
        if (!build_.keep_slide_on_3 || effect != kSlideToNote)
        {
            ch.linear_slide = 0;
        }

        ch.linear_vibrato = 0;
        ch.linear_slide_operand = 0;
        if (module_.amiga != 0)
        {
            ch.step = AmigaStep(ch);
        }
        else if (build_.vibrato == Mod30LinearForm::kAmigaOnly)
        {
            ch.step = LinearStep(int32_t(LinearPeriod(ch, ch.note)) + ch.linear_slide);
        }
        else if (build_.vibrato == Mod30LinearForm::kOwnPeriod)
        {
            ch.linear_period = LinearPeriod(ch, ch.note);
            Recompute(ch);
        }
        else
        {
            ch.note = uint16_t(LinearPeriod(ch, ch.note));
            Recompute(ch);
            ch.linear_target = 0;
        }

        if (effect != kSlideToNote)
        {
            ch.slide = 0;
        }
    }
    else if (sample != 0)
    {
        // A sample without a note: the channel takes its points and its volume, and keeps its place and lengths.
        ch.sample = uint16_t(sample - 1);
        ch.points = bank_.points[ch.sample];
        ch.volume = bank_.samples[ch.sample].volume;
        ch.packed = 0;
        ch.starts++;
        ch.first_point = ch.points;
    }

    ch.arpeggio_1 = 0;
    ch.arpeggio_2 = 0;
    if (effect <= 0xF)
    {
        RowEffect(ch, effect, d.operand);
    }

    VolumeColumn(ch);
}

void Mod30Player::RowEffect(Mod30Channel& ch, uint8_t effect, uint8_t operand)
{
    const bool linear = module_.amiga == 0;
    const uint16_t x = uint16_t(operand >> 4);
    const uint16_t y = uint16_t(operand & 0xF);
    switch (effect)
    {
    case kArpeggio:
        ch.effect = effect;
        ch.arpeggio_1 = uint16_t(x * 8);
        ch.arpeggio_2 = uint16_t(y * 8);
        break;

    case kSlideUp:
    case kSlideDown:
        ch.effect = effect;
        if (!linear)
        {
            ch.slide_step = int16_t(effect == kSlideUp ? -int32_t(operand) : int32_t(operand));
            break;
        }

        LinearSlide(ch, effect == kSlideUp, operand);
        Recompute(ch);
        break;

    case kSlideToNote:
        ch.effect = effect;
        if (operand != 0)
        {
            ch.slide_speed = operand;
        }

        if (!linear || build_.slide_to_note == Mod30LinearForm::kAmigaOnly)
        {
            ch.slide_step = ch.target == int16_t(ch.note)  ? 0
                            : ch.target > int32_t(ch.note) ? int16_t(-int32_t(ch.slide_speed))
                                                           : int16_t(ch.slide_speed);
        }
        else
        {
            ch.linear_target = LinearPeriod(ch, uint16_t(ch.target));
        }
        break;

    case kVibrato:
        ch.effect = effect;
        if (!build_.vibrato_nibbles)
        {
            if (operand != 0)
            {
                ch.vibrato_speed = x;
                ch.vibrato_depth = y;
            }
            break;
        }

        if (x != 0)
        {
            ch.vibrato_speed = x;
        }

        if (y != 0)
        {
            ch.vibrato_depth = y;
        }

        if (operand != 0)
        {
            ch.vibrato_position = 0;
        }
        break;

    case kSlideToNoteAndVolume:
    case kVibratoAndVolume:
    case kVolumeSlide:
        ch.effect = effect;
        ch.volume_up = x;
        ch.volume_down = y;
        break;

    case kJump:
        // The row is the order's last, and the order after it is the operand's.
        order_ = uint32_t(operand) - 1;
        row_ = module_.rows;
        break;

    case kVolume:
        ch.volume = operand;
        break;

    case kBreak:
        // The first break of a row moves to the next order. The streams don't seek: the next row reads on in them
        // unless the operand is 0.
        if (!broke_)
        {
            row_ = operand;
            order_++;
            if (order_ >= module_.order_count)
            {
                order_ = 0;
            }

            broke_ = 1;
        }
        break;

    case kExtended:
        if ((operand & 0xF0) == kPatternLoop)
        {
            if (y == 0)
            {
                ch.loop_row = uint16_t(row_ - 1);
            }
            else
            {
                ch.loop_count = ch.loop_count != 0 ? uint16_t(ch.loop_count - 1) : y;
                if (ch.loop_count != 0)
                {
                    row_ = ch.loop_row;
                }
            }
        }
        break;

    case kSpeed:
        SetSpeed(operand);
        break;

    default:
        break;
    }
}

void Mod30Player::SetSpeed(uint8_t operand)
{
    if (operand <= kLargestSpeed)
    {
        speed_ = operand;
    }
    else
    {
        tempo_ = operand;
    }

    hz_ = uint32_t(Divide(int32_t((tempo_ * 50) << 16), 125) >> 16);
    row_samples_ = Divide(int32_t(rate_ * speed_), int32_t(hz_));
    if (build_.speed_sets_tick)
    {
        tick_samples_ = Divide(int32_t(rate_), int32_t(hz_));
    }
}

void Mod30Player::Tick()
{
    if (playing_ == 0)
    {
        return;
    }

    const int count = build_.tick_all_channels ? channels_ : kTickChannels;
    for (int c = 0; c < count; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        TickEffect(ch);
        if (ch.arpeggio_1 + ch.arpeggio_2 == 0 && module_.amiga != 0)
        {
            ch.step = AmigaStep(ch);
        }

        if (build_.tick_column == Mod30TickColumn::kEvery)
        {
            VolumeColumn(ch);
        }
    }

    tick_++;
    arpeggio_tick_++;

    if (build_.tick_column == Mod30TickColumn::kRecordFour)
    {
        VolumeColumn(records_[kTickChannels]);
    }
    else if (build_.tick_column == Mod30TickColumn::kAfterModule && channels_ < build_.records)
    {
        VolumeColumn(records_[size_t(channels_)]);
    }
}

void Mod30Player::TickEffect(Mod30Channel& ch)
{
    switch (ch.effect)
    {
    case kArpeggio:
        // The arpeggio adds no interval.
        if (arpeggio_tick_ == 0 || ch.arpeggio_1 + ch.arpeggio_2 == 0)
        {
            break;
        }

        if (arpeggio_tick_ == 4)
        {
            arpeggio_tick_ = 1;
        }

        if (build_.linear_arpeggio && module_.amiga == 0)
        {
            ch.step = TableStep(ch.note);
        }
        else
        {
            ch.step = PeriodStep(int32_t(Period(uint32_t(ch.fine_tune + ch.note))) + ch.slide + ch.vibrato);
        }
        break;

    case kSlideUp:
    case kSlideDown:
        if (module_.amiga != 0)
        {
            ch.slide = int16_t(ch.slide + ch.slide_step);
            break;
        }

        LinearSlide(ch, ch.effect == kSlideUp, 0);
        Recompute(ch);
        break;

    case kSlideToNote:
        SlideToNote(ch);
        break;

    case kVibrato:
        Vibrato(ch);
        break;

    case kSlideToNoteAndVolume:
        SlideToNote(ch);
        VolumeSlide(ch);
        break;

    case kVibratoAndVolume:
        Vibrato(ch);
        VolumeSlide(ch);
        break;

    case kVolumeSlide:
        VolumeSlide(ch);
        break;

    default:
        break;
    }
}

void Mod30Player::LinearSlide(Mod30Channel& ch, bool up, uint32_t operand)
{
    if (operand != 0)
    {
        ch.linear_slide_operand = operand;
    }
    else
    {
        operand = ch.linear_slide_operand;
    }

    // The fine and extra-fine forms act at the row's first tick, the plain form at the others.
    const uint32_t high = (operand >> 4) & 0xFF;
    const uint32_t low = operand & 0xF;
    int32_t move = 0;
    if (high == kFine || high == kExtraFine)
    {
        if (tick_ != 0)
        {
            return;
        }

        move = int32_t(high == kFine ? low * 4 : low);
    }
    else
    {
        if (tick_ == 0)
        {
            return;
        }

        move = int32_t(operand & 0xFFFF) * build_.plain_slide_scale;
    }

    ch.linear_slide += up ? -move : move;
}

void Mod30Player::SlideToNote(Mod30Channel& ch)
{
    const bool linear = module_.amiga == 0;
    if (build_.slide_to_note == Mod30LinearForm::kAmigaOnly ||
        (!linear && build_.slide_to_note == Mod30LinearForm::kOwnPeriod))
    {
        // The Amiga form compares steps where periods were meant.
        ch.slide = int16_t(ch.slide + ch.slide_step);
        const uint32_t from = TableStep(ch.note) + uint32_t(int32_t(ch.slide));
        const uint32_t to = TableStep(uint32_t(int32_t(ch.target)));
        if (ch.slide_step > 0 ? from >= to : from <= to)
        {
            ch.note = uint16_t(ch.target);
            ch.slide_step = 0;
            ch.slide = 0;
        }

        return;
    }

    const int32_t speed = int32_t(ch.slide_speed) * 4;
    if (build_.slide_to_note == Mod30LinearForm::kOwnPeriod)
    {
        // The linear period moves towards the target, without an end.
        const int32_t d = int32_t(ch.linear_period) - int32_t(ch.linear_target);
        if (d == 0 || speed > std::abs(d))
        {
            ch.linear_period = ch.linear_target;
        }
        else
        {
            ch.linear_period = uint32_t(int32_t(ch.linear_period) + (d > 0 ? -speed : speed));
        }

        Recompute(ch);
        return;
    }

    if (ch.linear_target == 0)
    {
        return;
    }

    if (!linear)
    {
        ch.slide = int16_t(ch.slide + ch.slide_step);
        const uint32_t from = TableStep(ch.note) + uint32_t(int32_t(ch.slide));
        const uint32_t to = TableStep(uint32_t(int32_t(ch.target)));
        if (ch.slide_step > 0 ? from >= to : from <= to)
        {
            ch.note = uint16_t(ch.target);
            ch.slide_step = 0;
            ch.slide = 0;
        }

        return;
    }

    const int32_t target = int32_t(ch.linear_target);
    if (build_.slide_to_note == Mod30LinearForm::kNoteField)
    {
        // The period itself moves, and the slide ends at the target.
        const int32_t d = int32_t(ch.note) - target;
        if (d == 0 || int32_t(int16_t(ch.slide_speed)) * 4 > std::abs(d))
        {
            ch.note = uint16_t(target);
            ch.linear_target = 0;
        }
        else if (d > 0)
        {
            ch.note = uint16_t(ch.note - speed);
            if (int32_t(ch.note) <= target)
            {
                ch.note = uint16_t(target);
                ch.linear_target = 0;
            }
        }
        else
        {
            ch.note = uint16_t(ch.note + speed);
            if (int32_t(ch.note) >= target)
            {
                ch.note = uint16_t(target);
                ch.linear_target = 0;
            }
        }

        Recompute(ch);
        return;
    }

    // The linear slide's offset moves, and the slide ends at the target.
    const int32_t d = int32_t(ch.note) + ch.linear_slide - target;
    if (d == 0 || int32_t(int16_t(ch.slide_speed)) * 4 > std::abs(d))
    {
        ch.linear_slide = target - int32_t(ch.note);
        ch.linear_target = 0;
    }
    else if (d > 0)
    {
        ch.linear_slide -= speed;
        if (int32_t(ch.note) + ch.linear_slide <= target)
        {
            ch.linear_slide = target - int32_t(ch.note);
            ch.linear_target = 0;
        }
    }
    else
    {
        ch.linear_slide += speed;
        if (int32_t(ch.note) + ch.linear_slide >= target)
        {
            ch.linear_slide = target - int32_t(ch.note);
            ch.linear_target = 0;
        }
    }

    Recompute(ch);
}

void Mod30Player::Vibrato(Mod30Channel& ch)
{
    const bool linear = module_.amiga == 0;
    if (build_.vibrato == Mod30LinearForm::kAmigaOnly || build_.vibrato == Mod30LinearForm::kOwnPeriod)
    {
        // The first builds restart the vibrato at a row's first tick.
        if (tick_ == 0)
        {
            ch.vibrato_position = 0;
        }

        const uint16_t p = ch.vibrato_position;
        if (linear && build_.vibrato == Mod30LinearForm::kOwnPeriod)
        {
            const int32_t v = (kSine[(p >> 1) & 0x1F] * int32_t(ch.vibrato_depth)) >> 6;
            ch.linear_vibrato = v;
            ch.vibrato_position = uint16_t((p + ch.vibrato_speed) & 0x3F);
            Recompute(ch);
            return;
        }

        const int32_t wave = p < 32 ? kSine[p & 31] : -kSine[p & 31];
        ch.vibrato = int16_t((wave * int32_t(ch.vibrato_depth)) >> 7);
        ch.vibrato_position = uint16_t((p + ch.vibrato_speed) & 0x3F);
        return;
    }

    const uint16_t p = ch.vibrato_position;
    if (!linear)
    {
        ch.vibrato = int16_t((int32_t(ch.vibrato_depth) * kSine[p & 31]) >> 7);
        ch.vibrato_position = uint16_t((p + ch.vibrato_speed) & 0x3F);
        return;
    }

    const int32_t v = (kSine[(p >> 2) & 0x1F] * int32_t(ch.vibrato_depth)) >> 5;
    ch.linear_vibrato = p > 0x1F ? -v : v;
    ch.vibrato_position = uint16_t((p + ((ch.vibrato_speed + 4) >> 1)) & 0x3F);
    Recompute(ch);
}

void Mod30Player::VolumeSlide(Mod30Channel& ch)
{
    switch (build_.volume_slide)
    {
    case Mod30VolumeSlide::kOnce:
    case Mod30VolumeSlide::kTwice:
        {
            if (tick_ == 0 || uint16_t(ch.volume_up + ch.volume_down) == 0)
            {
                return;
            }

            const int32_t scale = build_.volume_slide == Mod30VolumeSlide::kTwice ? 2 : 1;
            ch.volume = ch.volume_up != 0 ? ch.volume + scale * ch.volume_up : ch.volume - scale * ch.volume_down;
            break;
        }

    case Mod30VolumeSlide::kOnceClamp:
        if (tick_ == 0)
        {
            return;
        }

        ch.volume = ch.volume_up != 0 ? ch.volume + ch.volume_up : ch.volume - ch.volume_down;
        break;

    case Mod30VolumeSlide::kOnceAlways:
        if (tick_ != 0)
        {
            ch.volume = ch.volume_up != 0 ? ch.volume + ch.volume_up : ch.volume - ch.volume_down;
        }
        break;
    }

    ch.volume = std::clamp(ch.volume, 0, kFullVolume);
}

void Mod30Player::VolumeColumn(Mod30Channel& ch)
{
    if (ch.volume_column != 0)
    {
        ch.volume = int32_t(ch.volume_column) - 1;
    }
}

uint32_t Mod30Player::LinearPeriod(const Mod30Channel& ch, uint32_t note) const
{
    const uint32_t semitones = uint32_t(ch.relative_note - 1 + int32_t(uint16_t(note)) - 1) & 0xFFFF;

    return uint32_t(kLinearBase - int32_t(semitones << kSemitoneShift) - (ch.fine_tune >> 1)) & 0xFFFF;
}

uint32_t Mod30Player::LinearStep(int32_t period) const
{
    const int32_t y = kLinearTop - period;
    const int32_t octaves = Divide(y, kOctave);
    const int32_t place =
        build_.vibrato == Mod30LinearForm::kAmigaOnly ? int32_t(uint32_t(y) % kOctave) : Remainder(y, kOctave);
    const uint32_t entry = rom_.U16(settings_.linear + uint32_t(place * 2));
    const uint32_t frequency = ShiftRight(entry << 2, uint32_t(7 - octaves));

    return uint32_t(Divide(int32_t(frequency << kFractionBits), int32_t(rate_)));
}

void Mod30Player::Recompute(Mod30Channel& ch)
{
    switch (build_.vibrato)
    {
    case Mod30LinearForm::kAmigaOnly:
        ch.step = LinearStep(int32_t(LinearPeriod(ch, ch.note)) + ch.linear_slide);
        break;

    case Mod30LinearForm::kOwnPeriod:
        ch.step = LinearStep(int32_t(ch.linear_period) + ch.linear_slide + ch.linear_vibrato);
        break;

    default:
        ch.step = LinearStep(int32_t(ch.note) + ch.linear_slide + ch.linear_vibrato);
        break;
    }
}

uint32_t Mod30Player::PeriodStep(int32_t period) const
{
    int32_t twice = period * 2;
    if (twice == 0)
    {
        twice = 1;
    }

    return uint32_t(Divide(Divide(kPeriodClock, twice) << kFractionBits, int32_t(rate_)));
}

uint32_t Mod30Player::AmigaStep(const Mod30Channel& ch) const
{
    if (ch.slide == 0 && ch.vibrato == 0 && ch.fine_tune == 0)
    {
        return TableStep(ch.note);
    }

    return PeriodStep(int32_t(Period(uint32_t(ch.fine_tune + ch.note))) + ch.slide + ch.vibrato);
}

uint16_t Mod30Player::Period(uint32_t pitch) const
{
    return rom_.U16(settings_.periods + pitch * 2);
}

uint32_t Mod30Player::TableStep(uint32_t pitch) const
{
    return rom_.U32(settings_.steps + pitch * 4);
}

void Mod30Player::Warn(const std::string& text)
{
    if (std::find(warnings_.begin(), warnings_.end(), text) == warnings_.end())
    {
        warnings_.push_back(text);
    }
}

} // namespace agblib::logikstate
