// SPDX-License-Identifier: MIT

// The records of the MOD-30 player in the layouts of the driver's builds, and the builds.

#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/logikstate/mod30.h"
#include "agblib/logikstate/mod30_player.h"

namespace agblib::logikstate
{
namespace
{

// The fields of a channel's record, and where each layout keeps them.
enum class Field
{
    kPoints,
    kPosition,
    kStep,
    kLength,
    kLoopStart,
    kLoopLength,
    kVolume,
    kRelativeNote,
    kSample,
    kFineTune,
    kNote,
    kEffect,
    kVolumeUp,
    kVolumeDown,
    kArpeggio1,
    kArpeggio2,
    kVibrato,
    kVibratoDepth,
    kVibratoSpeed,
    kVibratoPosition,
    kSlide,
    kSlideSpeed,
    kSlideStep,
    kTarget,
    kLoopRow,
    kLoopCount,
    kPacked,
    kLinearSlide,
    kLinearVibrato,
    kLinearSlideOperand,
    kVolumeColumn,
    kLinearPeriod,
    kLinearTarget,
};

struct FieldPlace
{
    Field field;
    uint32_t offset;
    uint32_t size;
};

// The fields from the volume on, after the six words that every layout starts with.
constexpr FieldPlace kWords[] = {
    {Field::kPoints, 0x00, 4},    {Field::kPosition, 0x04, 4},   {Field::kStep, 0x08, 4},   {Field::kLength, 0x0C, 4},
    {Field::kLoopStart, 0x10, 4}, {Field::kLoopLength, 0x14, 4}, {Field::kVolume, 0x18, 4},
};

constexpr FieldPlace kLayoutA[] = {
    {Field::kRelativeNote, 0x30, 2},    {Field::kSample, 0x34, 2},
    {Field::kFineTune, 0x38, 2},        {Field::kNote, 0x3A, 2},
    {Field::kEffect, 0x3C, 4},          {Field::kVolumeUp, 0x40, 2},
    {Field::kVolumeDown, 0x42, 2},      {Field::kArpeggio1, 0x44, 2},
    {Field::kArpeggio2, 0x46, 2},       {Field::kVibrato, 0x48, 2},
    {Field::kVibratoDepth, 0x4A, 2},    {Field::kVibratoSpeed, 0x4C, 2},
    {Field::kVibratoPosition, 0x4E, 2}, {Field::kSlide, 0x50, 2},
    {Field::kSlideSpeed, 0x52, 2},      {Field::kSlideStep, 0x54, 2},
    {Field::kTarget, 0x56, 2},          {Field::kLoopRow, 0x68, 2},
    {Field::kLoopCount, 0x6A, 2},       {Field::kPacked, 0x6C, 2},
    {Field::kLinearSlide, 0x70, 4},     {Field::kLinearSlideOperand, 0x74, 4},
    {Field::kVolumeColumn, 0x78, 4},
};

constexpr FieldPlace kLayoutB[] = {
    {Field::kRelativeNote, 0x30, 2},
    {Field::kSample, 0x34, 2},
    {Field::kFineTune, 0x38, 2},
    {Field::kNote, 0x3A, 2},
    {Field::kEffect, 0x3C, 4},
    {Field::kVolumeUp, 0x40, 2},
    {Field::kVolumeDown, 0x42, 2},
    {Field::kArpeggio1, 0x44, 2},
    {Field::kArpeggio2, 0x46, 2},
    {Field::kVibrato, 0x48, 2},
    {Field::kVibratoDepth, 0x4A, 2},
    {Field::kVibratoSpeed, 0x4C, 2},
    {Field::kVibratoPosition, 0x4E, 2},
    {Field::kSlide, 0x52, 2},
    {Field::kSlideSpeed, 0x54, 2},
    {Field::kSlideStep, 0x56, 2},
    {Field::kTarget, 0x58, 2},
    {Field::kLoopRow, 0x6C, 2},
    {Field::kLoopCount, 0x6E, 2},
    {Field::kPacked, 0x70, 2},
    {Field::kLinearSlide, 0x74, 4},
    {Field::kLinearVibrato, 0x78, 4},
    {Field::kLinearSlideOperand, 0x7C, 4},
    {Field::kVolumeColumn, 0x80, 4},
    {Field::kLinearPeriod, 0x88, 4},
    {Field::kLinearTarget, 0x8C, 4},
};

constexpr FieldPlace kLayoutC[] = {
    {Field::kRelativeNote, 0x38, 2},
    {Field::kSample, 0x3C, 2},
    {Field::kFineTune, 0x40, 2},
    {Field::kNote, 0x42, 2},
    {Field::kEffect, 0x44, 4},
    {Field::kVolumeUp, 0x48, 2},
    {Field::kVolumeDown, 0x4A, 2},
    {Field::kArpeggio1, 0x4C, 2},
    {Field::kArpeggio2, 0x4E, 2},
    {Field::kVibrato, 0x50, 2},
    {Field::kVibratoDepth, 0x52, 2},
    {Field::kVibratoSpeed, 0x54, 2},
    {Field::kVibratoPosition, 0x56, 2},
    {Field::kSlide, 0x5A, 2},
    {Field::kSlideSpeed, 0x5C, 2},
    {Field::kSlideStep, 0x5E, 2},
    {Field::kTarget, 0x60, 2},
    {Field::kLoopRow, 0x74, 2},
    {Field::kLoopCount, 0x76, 2},
    {Field::kPacked, 0x78, 2},
    {Field::kLinearSlide, 0x7C, 4},
    {Field::kLinearVibrato, 0x80, 4},
    {Field::kLinearSlideOperand, 0x84, 4},
    {Field::kVolumeColumn, 0x88, 4},
    {Field::kLinearTarget, 0x90, 4},
};

constexpr FieldPlace kLayoutE[] = {
    {Field::kRelativeNote, 0x3C, 2},
    {Field::kSample, 0x40, 2},
    {Field::kFineTune, 0x44, 2},
    {Field::kNote, 0x46, 2},
    {Field::kEffect, 0x48, 4},
    {Field::kVolumeUp, 0x4C, 2},
    {Field::kVolumeDown, 0x4E, 2},
    {Field::kArpeggio1, 0x50, 2},
    {Field::kArpeggio2, 0x52, 2},
    {Field::kVibrato, 0x54, 2},
    {Field::kVibratoDepth, 0x56, 2},
    {Field::kVibratoSpeed, 0x58, 2},
    {Field::kVibratoPosition, 0x5A, 2},
    {Field::kSlide, 0x5E, 2},
    {Field::kSlideSpeed, 0x60, 2},
    {Field::kSlideStep, 0x62, 2},
    {Field::kTarget, 0x64, 2},
    {Field::kLoopRow, 0x78, 2},
    {Field::kLoopCount, 0x7A, 2},
    {Field::kPacked, 0x7C, 2},
    {Field::kLinearSlide, 0x80, 4},
    {Field::kLinearVibrato, 0x84, 4},
    {Field::kLinearSlideOperand, 0x88, 4},
    {Field::kVolumeColumn, 0x8C, 4},
    {Field::kLinearTarget, 0x94, 4},
};

uint32_t RecordSize(Mod30RecordLayout layout)
{
    switch (layout)
    {
    case Mod30RecordLayout::kA:
        return 0x80;

    case Mod30RecordLayout::kB:
        return 0x90;

    case Mod30RecordLayout::kC:
        return 0x94;

    case Mod30RecordLayout::kE:
    default:
        return 0x98;
    }
}

uint32_t FieldValue(const Mod30Channel& ch, Field field)
{
    switch (field)
    {
    case Field::kPoints:
        return ch.points;
    case Field::kPosition:
        return ch.position;
    case Field::kStep:
        return ch.step;
    case Field::kLength:
        return ch.length;
    case Field::kLoopStart:
        return ch.loop_start;
    case Field::kLoopLength:
        return ch.loop_length;
    case Field::kVolume:
        return uint32_t(ch.volume);
    case Field::kRelativeNote:
        return uint16_t(ch.relative_note);
    case Field::kSample:
        return ch.sample;
    case Field::kFineTune:
        return uint16_t(ch.fine_tune);
    case Field::kNote:
        return ch.note;
    case Field::kEffect:
        return uint32_t(ch.effect);
    case Field::kVolumeUp:
        return ch.volume_up;
    case Field::kVolumeDown:
        return ch.volume_down;
    case Field::kArpeggio1:
        return ch.arpeggio_1;
    case Field::kArpeggio2:
        return ch.arpeggio_2;
    case Field::kVibrato:
        return uint16_t(ch.vibrato);
    case Field::kVibratoDepth:
        return ch.vibrato_depth;
    case Field::kVibratoSpeed:
        return ch.vibrato_speed;
    case Field::kVibratoPosition:
        return ch.vibrato_position;
    case Field::kSlide:
        return uint16_t(ch.slide);
    case Field::kSlideSpeed:
        return ch.slide_speed;
    case Field::kSlideStep:
        return uint16_t(ch.slide_step);
    case Field::kTarget:
        return uint16_t(ch.target);
    case Field::kLoopRow:
        return ch.loop_row;
    case Field::kLoopCount:
        return ch.loop_count;
    case Field::kPacked:
        return ch.packed;
    case Field::kLinearSlide:
        return uint32_t(ch.linear_slide);
    case Field::kLinearVibrato:
        return uint32_t(ch.linear_vibrato);
    case Field::kLinearSlideOperand:
        return ch.linear_slide_operand;
    case Field::kVolumeColumn:
        return ch.volume_column;
    case Field::kLinearPeriod:
        return ch.linear_period;
    case Field::kLinearTarget:
    default:
        return ch.linear_target;
    }
}

// Writes `value` into `out` at `offset` in `size` bytes, the lowest first.
void Put(std::vector<uint8_t>& out, uint32_t offset, uint32_t value, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
    {
        out[offset + i] = uint8_t(value >> (8 * i));
    }
}

void Append(std::vector<uint8_t>& out, uint32_t value, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
    {
        out.push_back(uint8_t(value >> (8 * i)));
    }
}

// The builds of the eight groups.
constexpr Mod30Build kBuildA = {
    .group = Mod30Group::kA,
    .record_layout = Mod30RecordLayout::kA,
    .records = 10,
    .channel_cap = 0,
    .mix_cap = 8,
    .loop_flag = false,
    .play_request = false,
    .frame_loop = Mod30FrameLoop::kShort,
    .tick_with_row = false,
    .speed_sets_tick = false,
    .short_lengths = true,
    .start_clears_records = true,
    .tick_all_channels = false,
    .tick_column = Mod30TickColumn::kRecordFour,
    .volume_slide = Mod30VolumeSlide::kOnce,
    .plain_slide_scale = 4,
    .vibrato = Mod30LinearForm::kAmigaOnly,
    .slide_to_note = Mod30LinearForm::kAmigaOnly,
    .vibrato_nibbles = false,
    .keep_slide_on_3 = false,
    .linear_arpeggio = false,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildB = {
    .group = Mod30Group::kB,
    .record_layout = Mod30RecordLayout::kB,
    .records = 8,
    .channel_cap = 0,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = false,
    .frame_loop = Mod30FrameLoop::kShort,
    .tick_with_row = false,
    .speed_sets_tick = false,
    .short_lengths = true,
    .start_clears_records = true,
    .tick_all_channels = false,
    .tick_column = Mod30TickColumn::kRecordFour,
    .volume_slide = Mod30VolumeSlide::kTwice,
    .plain_slide_scale = 4,
    .vibrato = Mod30LinearForm::kOwnPeriod,
    .slide_to_note = Mod30LinearForm::kOwnPeriod,
    .vibrato_nibbles = false,
    .keep_slide_on_3 = false,
    .linear_arpeggio = false,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildC = {
    .group = Mod30Group::kC,
    .record_layout = Mod30RecordLayout::kC,
    .records = 8,
    .channel_cap = 0,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = false,
    .frame_loop = Mod30FrameLoop::kShort,
    .tick_with_row = true,
    .speed_sets_tick = true,
    .short_lengths = true,
    .start_clears_records = true,
    .tick_all_channels = false,
    .tick_column = Mod30TickColumn::kRecordFour,
    .volume_slide = Mod30VolumeSlide::kOnceClamp,
    .plain_slide_scale = 1,
    .vibrato = Mod30LinearForm::kNoteField,
    .slide_to_note = Mod30LinearForm::kNoteField,
    .vibrato_nibbles = true,
    .keep_slide_on_3 = false,
    .linear_arpeggio = false,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildCPrime = {
    .group = Mod30Group::kCPrime,
    .record_layout = Mod30RecordLayout::kC,
    .records = 8,
    .channel_cap = 0,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = true,
    .frame_loop = Mod30FrameLoop::kShortList,
    .tick_with_row = true,
    .speed_sets_tick = true,
    .short_lengths = true,
    .start_clears_records = true,
    .tick_all_channels = false,
    .tick_column = Mod30TickColumn::kRecordFour,
    .volume_slide = Mod30VolumeSlide::kOnceClamp,
    .plain_slide_scale = 1,
    .vibrato = Mod30LinearForm::kNoteField,
    .slide_to_note = Mod30LinearForm::kNoteField,
    .vibrato_nibbles = true,
    .keep_slide_on_3 = false,
    .linear_arpeggio = false,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildD = {
    .group = Mod30Group::kD,
    .record_layout = Mod30RecordLayout::kC,
    .records = 12,
    .channel_cap = 12,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = true,
    .frame_loop = Mod30FrameLoop::kFull,
    .tick_with_row = true,
    .speed_sets_tick = true,
    .short_lengths = false,
    .start_clears_records = false,
    .tick_all_channels = true,
    .tick_column = Mod30TickColumn::kAfterModule,
    .volume_slide = Mod30VolumeSlide::kOnceAlways,
    .plain_slide_scale = 4,
    .vibrato = Mod30LinearForm::kNoteField,
    .slide_to_note = Mod30LinearForm::kSlideOffset,
    .vibrato_nibbles = true,
    .keep_slide_on_3 = true,
    .linear_arpeggio = true,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildE = {
    .group = Mod30Group::kE2,
    .record_layout = Mod30RecordLayout::kE,
    .records = 12,
    .channel_cap = 12,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = true,
    .frame_loop = Mod30FrameLoop::kFull,
    .tick_with_row = true,
    .speed_sets_tick = true,
    .short_lengths = false,
    .start_clears_records = false,
    .tick_all_channels = true,
    .tick_column = Mod30TickColumn::kEvery,
    .volume_slide = Mod30VolumeSlide::kOnceAlways,
    .plain_slide_scale = 4,
    .vibrato = Mod30LinearForm::kNoteField,
    .slide_to_note = Mod30LinearForm::kSlideOffset,
    .vibrato_nibbles = true,
    .keep_slide_on_3 = true,
    .linear_arpeggio = true,
    .scaled_zero_waits = true,
    .mode1_volume_zero = true,
};

constexpr Mod30Build WithGroup(Mod30Build build, Mod30Group group, int records)
{
    build.group = group;
    build.records = records;
    build.channel_cap = uint32_t(records);
    return build;
}

constexpr Mod30Build kBuildE1 = WithGroup(kBuildE, Mod30Group::kE1, 12);
constexpr Mod30Build kBuildE3 = WithGroup(kBuildE, Mod30Group::kE3, 8);

} // namespace

const Mod30Build& BuildOf(Mod30Group group)
{
    switch (group)
    {
    case Mod30Group::kA:
        return kBuildA;
    case Mod30Group::kB:
        return kBuildB;
    case Mod30Group::kC:
        return kBuildC;
    case Mod30Group::kCPrime:
        return kBuildCPrime;
    case Mod30Group::kD:
        return kBuildD;
    case Mod30Group::kE1:
        return kBuildE1;
    case Mod30Group::kE3:
        return kBuildE3;
    case Mod30Group::kE2:
    default:
        return kBuildE;
    }
}

std::vector<uint8_t> Mod30Player::Globals() const
{
    std::vector<uint8_t> out;
    Append(out, order_, 4);
    Append(out, row_, 4);
    Append(out, speed_, 4);
    Append(out, tempo_, 4);
    Append(out, hz_, 4);
    Append(out, uint32_t(row_samples_), 4);
    Append(out, uint32_t(row_left_), 4);
    Append(out, uint32_t(tick_samples_), 4);
    Append(out, uint32_t(tick_left_), 4);
    Append(out, tick_, 2);
    Append(out, arpeggio_tick_, 2);
    Append(out, broke_, 1);
    Append(out, playing_, 2);
    Append(out, fade_, 4);

    return out;
}

std::vector<uint8_t> Mod30Player::Record(int channel) const
{
    const Mod30Channel& ch = records_[size_t(channel)];
    std::vector<uint8_t> out(RecordSize(build_.record_layout), 0);
    for (const FieldPlace& f : kWords)
    {
        Put(out, f.offset, FieldValue(ch, f.field), f.size);
    }

    const auto put_all = [&](const auto& places)
    {
        for (const FieldPlace& f : places)
        {
            Put(out, f.offset, FieldValue(ch, f.field), f.size);
        }
    };

    switch (build_.record_layout)
    {
    case Mod30RecordLayout::kA:
        put_all(kLayoutA);
        break;
    case Mod30RecordLayout::kB:
        put_all(kLayoutB);
        break;
    case Mod30RecordLayout::kC:
        put_all(kLayoutC);
        break;
    case Mod30RecordLayout::kE:
        put_all(kLayoutE);
        break;
    }

    return out;
}

} // namespace agblib::logikstate
