// SPDX-License-Identifier: MIT

#include "agblib/frontline_xm/player.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"

namespace agblib::frontline_xm
{
namespace
{

// A note's period: 7680 less 64 a semitone, less half the instrument's finetune.
constexpr int32_t kTopPeriod = 7680;
constexpr int kSemitoneShift = 6;

// The period that the channels' reset sets, and the end of the stopped channel's point.
constexpr uint16_t kResetPeriod = 0x2000;
constexpr uint32_t kSilenceEnd = 0x2000;

// The mix of the four channels keeps bits 14 and up, clamped to a byte. A sum below -127 gives -128.
constexpr int kOutputShift = 14;

// The highest volume that the slides reach.
constexpr int32_t kMaxVolume = 64;

// The note handlers that start notes. Handlers 2 and 3 do nothing.
constexpr uint8_t kNoteHandlers = 2;
constexpr uint8_t kHandlerTable = 4;

// The highest effect in the player's table.
constexpr uint8_t kLastEffect = 30;

// The volume column's ranges.
constexpr int32_t kColumnVolume = 0x50;
constexpr int32_t kColumnSlideDown = 0x6F;
constexpr int32_t kColumnSlideUp = 0x7F;
constexpr int32_t kColumnFineDown = 0x8F;
constexpr int32_t kColumnFineUp = 0x9F;
constexpr int32_t kColumnVibratoSpeed = 0xAF;
constexpr int32_t kColumnVibratoDepth = 0xBF;
constexpr int32_t kColumnSlideToNote = 0xFF;

uint8_t Low(uint8_t v)
{
    return v & 15;
}

uint8_t High(uint8_t v)
{
    return v >> 4;
}

} // namespace

Player::Player(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
    frame_samples_ = uint16_t(info.FrameSamples(rom));
    sums_.assign(frame_samples_, 0);
    output_.assign(frame_samples_, 0);
}

const std::vector<int8_t>& Player::Output() const
{
    return output_;
}

uint8_t Player::Order() const
{
    return order_;
}

uint8_t Player::Row() const
{
    return row_;
}

uint8_t Player::TickCount() const
{
    return tick_;
}

uint8_t Player::Speed() const
{
    return speed_;
}

uint8_t Player::Bpm() const
{
    return bpm_;
}

uint8_t Player::Master() const
{
    return master_;
}

uint16_t Player::TickSamples() const
{
    return tick_samples_;
}

uint16_t Player::SamplesToTick() const
{
    return left_;
}

uint32_t Player::FrameSamples() const
{
    return frame_samples_;
}

const Channel& Player::ChannelAt(int c) const
{
    return channels_[c];
}

uint64_t Player::Elapsed() const
{
    return elapsed_;
}

bool Player::RowRead() const
{
    return row_read_;
}

uint8_t Player::RowOrder() const
{
    return row_order_;
}

uint8_t Player::RowNumber() const
{
    return row_number_;
}

int Player::Unmodelled() const
{
    return unmodelled_;
}

uint16_t Player::SamplesPerTick(uint8_t bpm)
{
    if (bpm == 0)
    {
        unmodelled_++;
        return tick_samples_;
    }

    return uint16_t(uint32_t(info_.Rate(rom_)) * 5 / (2 * uint32_t(bpm)));
}

uint16_t Player::Period(uint8_t note, uint32_t instrument) const
{
    return uint16_t(kTopPeriod - (int32_t(note) << kSemitoneShift) -
                    (int32_t(rom_.S8(instrument + kInstrumentFinetune)) >> 1));
}

void Player::Start(int song)
{
    const Song& s = info_.songs.at(size_t(song));
    Load(info_.modules.at(size_t(s.module)));
    Play(s.list, 0, s.last);
    master_ = s.volume;
}

// The load (0x080DFB7C): the header's fields, the samples a tick, the waves, the channels' reset, and the play of the
// module's list.
void Player::Load(const Module& module)
{
    channel_count_ = module.channels;
    master_ = module.master;
    last_ = module.last_order;
    speed_ = module.speed;
    tick_ = module.speed;
    list_ = module.orders;
    patterns_ = module.patterns;
    instruments_ = module.instruments;
    bpm_ = module.bpm;
    tick_samples_ = SamplesPerTick(module.bpm);
    note_handler_ = module.note_handler;
    vibrato_wave_ = info_.library.waves[0];
    tremolo_wave_ = info_.library.waves[0];
    break_row_ = 0;
    pattern_delay_ = 0;
    left_ = 0;
    jump_order_ = 0;

    // The reset (0x080DFD58) leaves the end, the period, the cell, the counters, the arpeggio and the loop type.
    for (int i = 0; i < channel_count_ && i < kChannels; i++)
    {
        Channel& c = channels_[i];
        c.instrument = 0;
        c.points = 0;
        c.position = 0;
        c.step = 0;
        c.sounding = kResetPeriod;
        c.target = 0;
        c.slide_up = 0;
        c.slide_down = 0;
        c.slide_speed = 0;
        c.volume_slide = 0;
        c.volume = 0;
        c.effect = Routine::kNone;
        c.column = Routine::kNone;
        c.vibrato_place = 0;
        c.vibrato_speed = 0;
        c.vibrato_depth = 0;
        c.tremolo_place = 0;
        c.tremolo_speed = 0;
        c.tremolo_depth = 0;
        c.on = 0;
    }

    Play(module.orders, 0, module.last_order);
}

// The play (0x080E040C).
void Player::Play(uint32_t list, uint8_t first, uint8_t last)
{
    list_ = list;
    last_ = last;
    LoadPattern(first);
    tick_ = 0;
}

void Player::LoadPattern(uint8_t order)
{
    order_ = order;
    const uint32_t pattern = rom_.U32(patterns_ + 4 * rom_.U8(list_ + order));
    rows_ = uint8_t(rom_.U16(pattern));
    row_ = 0;
    data_ = pattern + kPatternCells;
}

// The mix routine (0x080E5974): ticks and segments to the frame's end.
void Player::Frame()
{
    const uint8_t master = master_;
    uint32_t remaining = frame_samples_;
    uint32_t left = left_;
    uint32_t at = 0;
    for (;;)
    {
        const bool ticked = left == 0;
        if (ticked)
        {
            Tick();
            left = tick_samples_;
        }

        SetSteps();
        if (ticked && on_tick)
        {
            on_tick();
        }

        const uint32_t count = std::min(remaining, left);
        MixSegment(at, count, master);
        elapsed_ += count;
        if (remaining <= left)
        {
            left_ = uint16_t(left - remaining);
            break;
        }

        remaining -= left;
        at += left;
        left = 0;
    }

    for (uint32_t i = 0; i < frame_samples_; i++)
    {
        const int32_t v = sums_[i] >> kOutputShift;
        output_[i] = int8_t(v > 127 ? 127 : v < -127 ? -128 : v);
    }
}

// The tick (0x080E5A44): a row once the count reaches the speed, else the channels' routines (0x080E61EC).
void Player::Tick()
{
    row_read_ = false;
    if (tick_ < speed_)
    {
        tick_++;
        for (int i = 0; i < channel_count_ && i < kChannels; i++)
        {
            Channel& c = channels_[i];
            RunRoutine(c, c.effect);
            RunRoutine(c, c.column);
        }

        return;
    }

    tick_ = 1;
    ReadRow();
}

// The row (0x080E5A80).
void Player::ReadRow()
{
    row_read_ = true;
    row_order_ = order_;
    row_number_ = row_;
    uint32_t at = data_;
    for (int i = 0; i < channel_count_; i++)
    {
        if (i >= kChannels)
        {
            unmodelled_++;
            break;
        }

        Channel& c = channels_[i];
        c.cell = at;
        const uint8_t flags = rom_.U8(at++);
        uint8_t note = 0;
        uint8_t instrument = 0;
        if (flags & kCellNote)
        {
            note = rom_.U8(at++);
            instrument = rom_.U8(at++);
        }

        int32_t column = flags & kCellVolume ? rom_.U8(at++) : 0;
        uint8_t effect = 0;
        uint8_t parameter = 0;
        if (flags & kCellEffect)
        {
            effect = rom_.U8(at++);
            parameter = rom_.U8(at++);
        }

        if (flags & kCellNote)
        {
            if (note_handler_ < kNoteHandlers)
            {
                Note(c, note, instrument, effect, column);
            }
            else if (note_handler_ >= kHandlerTable)
            {
                unmodelled_++;
            }
        }
        else if (c.column != Routine::kVibrato)
        {
            c.sounding = c.period;
        }

        c.effect = Routine::kNone;
        c.column = Routine::kNone;
        if (column != 0)
        {
            column = Column(c, column);
        }

        RunEffect(c, effect, parameter, column);
    }

    data_ = at;

    const int next = row_ + 1;
    row_ = uint8_t(next);
    if (next < rows_)
    {
        return;
    }

    int order = order_ + 1;
    if (order > last_)
    {
        order = restart_;
    }

    LoadPattern(uint8_t(order));
    if (break_row_ != 0)
    {
        const uint8_t row = break_row_;
        break_row_ = 0;
        if (row <= rows_)
        {
            row_ = row;
            at = data_;
            for (int r = 0; r < row; r++)
            {
                for (int i = 0; i < channel_count_; i++)
                {
                    const uint8_t flags = rom_.U8(at++);
                    at += (flags & kCellNote ? 2 : 0) + (flags & kCellEffect ? 2 : 0) + (flags & kCellVolume ? 1 : 0);
                }
            }

            data_ = at;
        }
    }
}

// The note handler (0x080E5C14). A slide to the note keeps the note as the target. A delayed note keeps nothing and
// drops the volume column.
void Player::Note(Channel& c, uint8_t note, uint8_t instrument, uint8_t effect, int32_t& column)
{
    const uint32_t record = rom_.U32(instruments_ + 4 * instrument);
    const uint16_t period = Period(note, record);
    if (effect == kEffectSlideToNote || effect == kEffectSlideAndVolume)
    {
        c.target = period;
        return;
    }

    if (effect == kEffectDelay)
    {
        column = 0;
        return;
    }

    c.period = period;
    StartNote(c, note, instrument);
}

// The fields of a note's start, for the note handler and the delayed note. Sets the sounding period, not the channel's
// period.
void Player::StartNote(Channel& c, uint8_t note, uint8_t instrument)
{
    const uint32_t record = rom_.U32(instruments_ + 4 * instrument);
    c.sounding = Period(note, record);
    c.instrument = record;
    c.volume = rom_.U8(record + kInstrumentVolume);
    c.end = rom_.U32(record + kInstrumentLength);
    c.loop_type = rom_.U8(record + kInstrumentLoopType);
    c.points = record + kInstrumentPoints;
    c.position = 0;
    c.vibrato_place = 0;
    c.tremolo_place = 0;
    c.on = 1;
    c.starts++;
    c.number = instrument;
}

// The volume column (0x080E5D7C). Returns the value as the code leaves it in r0. Effect 8 reads that value.
int32_t Player::Column(Channel& c, int32_t value)
{
    if (value <= kColumnVolume)
    {
        value -= 16;
        c.volume = uint8_t(value);
    }
    else if (value <= kColumnSlideDown)
    {
        value = -(value - 96);
        c.volume_slide = int8_t(uint8_t(value));
        c.column = Routine::kVolumeSlide;
    }
    else if (value <= kColumnSlideUp)
    {
        value -= 112;
        c.volume_slide = int8_t(uint8_t(value));
        c.column = Routine::kVolumeSlide;
    }
    else if (value <= kColumnFineDown)
    {
        value -= 128;
        c.volume = uint8_t(std::max<int32_t>(c.volume - value, 0));
    }
    else if (value <= kColumnFineUp)
    {
        value -= 144;
        c.volume = uint8_t(std::min<int32_t>(c.volume + value, kMaxVolume));
    }
    else if (value <= kColumnVibratoSpeed)
    {
        value -= 160;
        c.vibrato_speed = uint8_t(value);
    }
    else if (value <= kColumnVibratoDepth)
    {
        value -= 176;
        c.vibrato_depth = uint8_t(value);
        c.column = Routine::kVibrato;
    }
    else if (value <= kColumnSlideToNote)
    {
        // From 0xC0, not only from 0xF0: the code tests no lower bound.
        value -= 240;
        c.slide_speed = uint8_t(value);
        c.column = Routine::kSlideToNote;
    }

    return value;
}

// The effects' handlers (the table at 0x080E60E0).
void Player::RunEffect(Channel& c, uint8_t effect, uint8_t parameter, int32_t column)
{
    const uint8_t p = parameter;
    switch (effect)
    {
    case kEffectBpm:
        bpm_ = p;
        tick_samples_ = SamplesPerTick(p);
        break;
    case kEffectSlideUp:
        if (p != 0)
        {
            c.slide_up = p;
        }

        c.effect = Routine::kSlideUp;
        break;
    case kEffectSlideDown:
        if (p != 0)
        {
            c.slide_down = p;
        }

        c.effect = Routine::kSlideDown;
        break;
    case kEffectSlideToNote:
        if (p != 0)
        {
            c.slide_speed = p;
        }

        c.effect = Routine::kSlideToNote;
        break;
    case kEffectVolume:
        c.volume = p;
        break;
    case kEffectVolumeSlide:
    case kEffectSlideAndVolume:
    case kEffectVibratoAndVolume:
        {
            // A low nibble slides down by the whole parameter.
            const uint8_t memory = Low(p) != 0 ? uint8_t(-p) : High(p);
            if (memory != 0)
            {
                c.volume_slide = int8_t(memory);
            }

            if (effect == kEffectVolumeSlide)
            {
                c.effect = Routine::kVolumeSlide;
            }
            else
            {
                c.effect = Routine::kRepeatTick;
                unmodelled_++;
            }
            break;
        }
    case kEffectVibrato:
        if (p != 0)
        {
            c.vibrato_speed = High(p);
            c.vibrato_depth = Low(p);
        }

        c.effect = Routine::kVibrato;
        break;
    case kEffectVolumeColumn:
        Column(c, column);
        break;
    case kEffectSpeed:
        speed_ = p;
        break;
    case kEffectArpeggio:
        c.arpeggio = p;
        c.counter = 0;
        c.effect = Routine::kArpeggio;
        break;
    case kEffectTremolo:
        if (p != 0)
        {
            c.tremolo_speed = High(p);
            c.tremolo_depth = Low(p);
        }

        c.effect = Routine::kTremolo;
        break;
    case kEffectJump:
        jump_order_ = p;
        c.effect = Routine::kJump;
        break;
    case kEffectOffset:
        {
            const int32_t position = int32_t(uint32_t(p) << (8 + kPositionShift));
            if (position < int32_t(c.end))
            {
                c.position = uint32_t(position);
                c.starts++;
            }
            break;
        }
    case kEffectBreak:
        break_row_ = p;
        row_ = rows_;
        break;
    case kEffectFineSlideUp:
        c.sounding = uint16_t(c.sounding - (p << 4));
        break;
    case kEffectFineSlideDown:
        c.sounding = uint16_t(c.sounding + (p >> 4));
        break;
    case kEffectVibratoWave:
        vibrato_wave_ = p != 0 ? info_.library.waves[0] : info_.library.waves[2];
        break;
    case kEffectTremoloWave:
        tremolo_wave_ = p != 0 ? info_.library.waves[0] : info_.library.waves[2];
        break;
    case kEffectRetrigger:
        c.retrigger = p;
        c.counter = 0;
        c.effect = Routine::kRetrigger;
        break;
    case kEffectFineVolumeUp:
        c.volume = uint8_t(std::min<int32_t>(c.volume + p, kMaxVolume));
        break;
    case kEffectFineVolumeDown:
        c.volume = uint8_t(std::max<int32_t>(c.volume - p, 0));
        break;
    case kEffectCut:
        c.counter = p;
        c.effect = Routine::kCut;
        break;
    case kEffectDelay:
        c.counter = p;
        c.effect = note_handler_ < kNoteHandlers ? Routine::kDelay : Routine::kDelayNothing;
        break;
    case kEffectPatternDelay:
        pattern_delay_ = p;
        break;
    default:
        if (effect > kLastEffect)
        {
            unmodelled_++;
        }
        break;
    }
}

// The tick routines (from 0x080E6234).
void Player::RunRoutine(Channel& c, Routine routine)
{
    switch (routine)
    {
    case Routine::kNone:
        break;
    case Routine::kSlideUp:
        c.period = uint16_t(c.period - (c.slide_up << 2));
        c.sounding = c.period;
        break;
    case Routine::kSlideDown:
        c.period = uint16_t(c.period + (c.slide_down << 2));
        c.sounding = c.period;
        break;
    case Routine::kSlideToNote:
        {
            // On arrival it stops the channel's effect, even when it runs as the volume column's routine.
            int32_t period = c.period;
            const int32_t target = c.target;
            const int32_t step = int32_t(c.slide_speed) << 2;
            if (target < period)
            {
                period -= step;
                if (period <= target)
                {
                    period = target;
                    c.effect = Routine::kNone;
                }
            }
            else
            {
                period += step;
                if (period >= target)
                {
                    period = target;
                    c.effect = Routine::kNone;
                }
            }

            c.period = uint16_t(period);
            c.sounding = c.period;
            break;
        }
    case Routine::kVolumeSlide:
        c.volume = uint8_t(std::clamp<int32_t>(int32_t(c.volume) + c.volume_slide, 0, kMaxVolume));
        break;
    case Routine::kVibrato:
        {
            c.vibrato_place = uint8_t(c.vibrato_place + (c.vibrato_speed << 2));
            const int32_t delta = (int32_t(rom_.S8(vibrato_wave_ + c.vibrato_place)) * c.vibrato_depth) >> 3;
            c.sounding = uint16_t(c.period + delta);
            break;
        }
    case Routine::kArpeggio:
        {
            uint16_t period = c.period;
            if (c.counter == 0)
            {
                period = uint16_t(period - (High(c.arpeggio) << kSemitoneShift));
                c.counter++;
            }
            else if (c.counter == 1)
            {
                period = uint16_t(period - (Low(c.arpeggio) << kSemitoneShift));
                c.counter++;
            }

            c.sounding = period;
            break;
        }
    case Routine::kRepeatTick:
        break;
    case Routine::kCut:
        if (c.counter != 0)
        {
            c.counter--;
            break;
        }

        c.volume = 0;
        c.effect = Routine::kNone;
        break;
    case Routine::kRetrigger:
        // It counts the ticks and plays nothing.
        c.counter = c.counter >= c.retrigger ? 1 : uint8_t(c.counter + 1);
        break;
    case Routine::kTremolo:
        {
            c.tremolo_place = uint8_t(c.tremolo_place + (c.tremolo_speed << 2));
            const int32_t delta = (int32_t(rom_.S8(tremolo_wave_ + c.tremolo_place)) * c.tremolo_depth) >> 4;
            c.volume = uint8_t(std::clamp<int32_t>(int32_t(c.volume) + delta, 0, kMaxVolume));
            break;
        }
    case Routine::kJump:
        LoadPattern(jump_order_ > last_ ? 0 : jump_order_);
        c.effect = Routine::kNone;
        break;
    case Routine::kDelay:
        if (c.counter != 0)
        {
            c.counter--;
            break;
        }

        DelayedNote(c);
        c.effect = Routine::kNone;
        break;
    case Routine::kDelayNothing:
        if (c.counter != 0)
        {
            c.counter--;
            break;
        }

        c.effect = Routine::kNone;
        break;
    }
}

// The delayed note (0x080E65A4) reads the row's cell again. It drops the volume column and leaves the channel's
// period: the next row without a note goes back to it.
void Player::DelayedNote(Channel& c)
{
    uint32_t at = c.cell;
    const uint8_t flags = rom_.U8(at++);
    if (flags & kCellNote)
    {
        const uint8_t note = rom_.U8(at++);
        const uint8_t instrument = rom_.U8(at++);
        StartNote(c, note, instrument);
    }
}

// The segment's set-up (0x080E05B0): each channel's step from its sounding period.
void Player::SetSteps()
{
    for (Channel& c : channels_)
    {
        c.step = StepOf(rom_, info_, c.sounding);
    }
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

// The mixer in IWRAM (0x03000268): channels 0 and 1 store the sums, then channels 2 and 3 add to them. A channel
// whose position reaches its end loops to the instrument's loop start, keeping the overshoot, or stops on the
// silence.
void Player::MixSegment(uint32_t from, uint32_t count, uint8_t master)
{
    const Library& lib = info_.library;
    for (int i = 0; i < kChannels; i++)
    {
        Channel& c = channels_[i];
        const int32_t gain = int32_t(c.volume) * master;
        uint32_t step = c.on != 0 ? c.step : 0;
        for (uint32_t n = 0; n < count; n++)
        {
            int32_t point = 0;
            if (c.instrument == 0)
            {
                // A channel that has played no note reads the BIOS. The model doesn't follow that read.
                if (gain != 0)
                {
                    unmodelled_++;
                }
            }
            else
            {
                if (c.position >= c.end)
                {
                    if (rom_.U8(c.instrument + kInstrumentLoopType) != 0)
                    {
                        c.end = rom_.U32(c.instrument + kInstrumentLoopEnd);
                        c.position = rom_.U32(c.instrument + kInstrumentLoopStart) + (c.position - c.end);
                    }
                    else
                    {
                        c.on = 0;
                        c.points = lib.silence;
                        c.end = kSilenceEnd;
                        step = 0;
                        c.position = 0;
                    }
                }

                point = Point(c.points + (c.position >> kPositionShift));
            }

            c.position += step;
            int32_t& sum = sums_[from + n];
            sum = (i == 0 ? 0 : sum) + point * gain;
        }
    }
}

} // namespace agblib::frontline_xm
