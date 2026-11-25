// SPDX-License-Identifier: MIT

// The model's set-up, its starts, its frames and its sequencer: the rows and the effects. The mixer, the envelopes and
// the volume and pitch are in player_mix.cpp. Each routine names its address in B33E.

#include "agblib/engine_software/player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"

namespace agblib::engine_software
{
namespace
{

// The sequencer's flags.
constexpr uint16_t kSongPlays = 0x0001;
constexpr uint16_t kSongPaused = 0x0002;
constexpr uint16_t kSongEnded = 0x0008;
constexpr uint16_t kSongJingle = 0x0100;

// The bytes of each of the mixer's two output buffers.
constexpr uint32_t kBufferBytes = 0x1C0;

// The values of the volume column.
constexpr uint16_t kColumnVolume = 0x10; // 0x10 to 0x50 sets the volume
constexpr uint16_t kColumnFine = 0x0E;   // the span of each group of slides and moves after its first value

// The init's count of music channels before it sets 0. The count sizes the first clip table.
constexpr int kInitChannels = 4;

// The pan of an instrument, from 0 to 255, as the driver keeps it.
int8_t InstrumentPan(uint8_t pan)
{
    return int8_t((pan >> 1) - 0x40);
}

// Adds `delta` to a channel's volume and keeps it within 0 to 64, as the driver's slides do with the byte's sign.
int8_t SlideVolume(int8_t volume, int delta)
{
    const int8_t v = int8_t(uint8_t(volume) + delta);
    if (v < 0)
    {
        return 0;
    }

    return v > 0x40 ? int8_t(0x40) : v;
}

} // namespace

Player::Player(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
    SetUp();
}

// The game's set-up of the manager and the driver: the manager sets its music volume, then the reset clears the
// driver's state, the init (0x08001108) stops every channel, sizes the clip table for 4 channels and sets the music
// volume to 256, and the effect set adds the effect channels.
void Player::SetUp()
{
    channels_ = {};
    for (int c = 0; c < kMaxRecords; c++)
    {
        channels_[size_t(c)].flags = kChannelOff;
    }

    output_on_ = false;
    music_ = kInitChannels;
    effects_ = 0;
    BuildClipTable();
    music_ = 0;
    effects_ = int(std::min<uint32_t>(info_.build.effect_channels, kMaxRecords));
    for (std::vector<int8_t>& buffer : buffers_)
    {
        buffer.assign(kBufferBytes, 0);
    }

    buffer_ = 0;
    written_ = 0;
    flags_ = 0;
    tick_ = 0;
    speed_ = 0;
    pattern_delay_ = 0;
    row_ = 0;
    left_ = 0;
    music_volume_ = 0x100;
    patterns_.clear();
    pattern_ = 0;
    cells_ = {};
    manager_volume_ = int32_t(info_.build.manager.music_volume);
    manager_step_ = 0;
    manager_target_ = 0;
    output_.assign(info_.FrameSamples(), 0);
    unmodelled_ = 0;
    elapsed_ = 0;
    row_read_ = false;
}

void Player::Start(int song)
{
    SetUp();
    const SongStart& start = info_.songs[size_t(song)].start;
    if (start.jingle)
    {
        // The game plays a jingle over a song that plays at the manager's music volume (0x0802CC7C).
        SetMusicVolume(uint16_t(info_.build.manager.music_volume));
        PlaySong(song, kSongJingle | kSongPlays);
        return;
    }

    // The manager's start (0x0802CBE8): the music volume to 0, then a ramp toward the target in steps of the
    // difference over the frames, rounded away from 0.
    if (manager_volume_ != 0)
    {
        SetMusicVolume(0);
        manager_volume_ = 0;
    }

    const int32_t target = int32_t(info_.build.manager.music_volume);
    const int32_t frames = int32_t(start.frames);
    if (frames == 0)
    {
        if (manager_volume_ != target)
        {
            SetMusicVolume(uint16_t(target));
            manager_volume_ = target;
        }

        manager_step_ = 0;
    }
    else if (target != manager_volume_)
    {
        const int32_t difference = target - manager_volume_;
        int32_t step = difference / frames;
        if (difference % frames != 0)
        {
            step = step < 0 ? step - 1 : step + 1;
        }

        manager_step_ = step;
        manager_target_ = target;
    }

    PlaySong(song, kSongPlays);
}

// The driver's music volume (0x08001334): every music channel that plays works out its level again.
void Player::SetMusicVolume(uint16_t volume)
{
    music_volume_ = volume;
    MarkVolumes();
}

void Player::MarkVolumes()
{
    for (int c = 0; c < music_; c++)
    {
        Channel& ch = channels_[size_t(c)];
        if (!(ch.flags & kChannelOff))
        {
            ch.flags |= kChannelVolume;
        }
    }
}

// The count of music channels (0x08000218), from the set-up's 0: at most 8, with the effect channels after them, and
// a clip table for the count.
void Player::SetMusicChannels(int count)
{
    if (count <= music_)
    {
        unmodelled_++;
        return;
    }

    music_ = std::min(count, 8);
    effects_ = std::min(effects_, kMaxRecords - music_);
    BuildClipTable();
}

// The play (0x08000980) of a song by its number (0x08001294).
void Player::PlaySong(int song, uint16_t flags)
{
    const Song& s = info_.songs[size_t(song)];
    if (flags_ & kSongPlays)
    {
        unmodelled_++;
    }

    tick_ = 0;
    left_ = 0;
    passes_ = 0;
    tempo_ = s.tempo;
    speed_ = s.speed;
    song_channels_ = s.channels;
    tick_samples_ = uint16_t(engine_software::TickSamples(StoredRate(info_.build.rate), tempo_));
    order_ = 0xFF;
    order_list_ = s.order_list;
    last_ = uint8_t(s.orders - 1);
    restart_ = s.restart;
    row_bytes_ = uint8_t((kRowFields * s.channels + 7) >> 3);
    patterns_ = s.patterns;
    NextOrder(0);
    SetMusicChannels(s.channels);
    for (int c = 0; c < music_; c++)
    {
        channels_[size_t(c)].flags = kChannelOff;
    }

    // The output's start (0x08000784) clears both buffers and points the mixer at the first.
    if (!output_on_)
    {
        for (std::vector<int8_t>& buffer : buffers_)
        {
            std::fill(buffer.begin(), buffer.end(), int8_t(0));
        }

        buffer_ = 0;
        written_ = 0;
        output_on_ = true;
    }

    flags_ = flags;
}

// The stop (0x08000AB4) at a jingle's end: every music channel stops, the count of music channels goes to 0 and the
// song's flags with it. The effect channels keep the output going.
void Player::Stop()
{
    for (int c = 0; c < music_; c++)
    {
        channels_[size_t(c)].flags = kChannelOff;
    }

    if (effects_ == 0)
    {
        unmodelled_++;
    }

    music_ = 0;
    flags_ = 0;
    left_ = 0;
    patterns_.clear();
}

void Player::Frame()
{
    // The manager's routine of the vertical count (0x0802CEC8): a step of the music volume's ramp (0x0802CAC4), then
    // the frame routine.
    if (manager_step_ != 0)
    {
        int32_t volume = manager_volume_ + manager_step_;
        if (manager_step_ < 0 ? volume <= manager_target_ : volume >= manager_target_)
        {
            volume = manager_target_;
            manager_step_ = 0;
        }

        if (volume != manager_volume_)
        {
            SetMusicVolume(uint16_t(volume));
            manager_volume_ = volume;
        }
    }

    RunFrame();
    const std::vector<int8_t>& buffer = buffers_[size_t(buffer_)];
    output_.assign(buffer.begin(), buffer.begin() + std::ptrdiff_t(info_.FrameSamples()));

    // The vertical blank's swap (0x08001174): the DMA plays the buffer that the mixer filled, and the mixer fills the
    // other.
    if (output_on_)
    {
        buffer_ ^= 1;
        written_ = 0;
    }
}

// The frame routine (0x08000C14). While a song plays, it runs a tick whenever the samples to the next tick reach 0 and
// mixes the frame in pieces between the ticks. Otherwise it mixes the effect channels alone, and stops the music once
// a jingle has ended.
void Player::RunFrame()
{
    const uint32_t frame = info_.FrameSamples();
    if ((flags_ & (kSongPlays | kSongPaused | kSongEnded)) == kSongPlays)
    {
        uint32_t remaining = frame;
        while (remaining != 0)
        {
            if (left_ == 0)
            {
                left_ = tick_samples_;
                Tick();
                if (left_ == 0)
                {
                    unmodelled_++;
                    return;
                }
            }

            const uint32_t piece = std::min<uint32_t>(left_, remaining);
            remaining -= piece;
            left_ = uint16_t(left_ - piece);
            BuildChain();
            Mix(piece);
        }

        return;
    }

    if (effects_ != 0)
    {
        BuildChain();
        Mix(frame);
    }

    if (flags_ & kSongEnded)
    {
        Stop();
    }
}

// A tick: the envelopes and fade-outs, then a row on the speed's first tick or the effects on the others.
void Player::Tick()
{
    Envelopes();
    row_read_ = false;
    if (tick_ == 0)
    {
        ReadRow();
    }
    else
    {
        for (int c = 0; c < music_; c++)
        {
            TickEffect(channels_[size_t(c)], cells_[size_t(c)]);
        }
    }

    tick_++;
    if (tick_ >= speed_)
    {
        tick_ = 0;
    }

    if (on_tick)
    {
        on_tick();
    }
}

// The next order (0x08001E00), from row `row`: past the last order, a jingle ends and another song goes to its
// restart and counts a pass.
void Player::NextOrder(uint16_t row)
{
    order_++;
    if (order_ > last_)
    {
        if (flags_ & kSongJingle)
        {
            flags_ |= kSongEnded;
        }
        else
        {
            passes_++;
            order_ = restart_;
        }
    }

    row_ = row;
    const uint8_t pattern = rom_.U8(order_list_ + order_);
    if (pattern >= patterns_.size())
    {
        unmodelled_++;
        return;
    }

    pattern_ = patterns_[pattern];
}

// The row (0x08001970): unless a pattern delay holds it, it unpacks the row's fields into the cells and runs each music
// channel's routines. An empty row clears the cells.
void Player::ReadRow()
{
    if (pattern_delay_ != 0)
    {
        pattern_delay_--;
        return;
    }

    const uint16_t r = row_;
    row_++;
    row_read_ = true;
    row_order_ = order_;
    row_number_ = r;
    const uint32_t offset = rom_.U32(pattern_ + kPatternRows + 4 * uint32_t(r));
    if (song_channels_ > cells_.size())
    {
        unmodelled_++;
        return;
    }

    if (offset == 0)
    {
        std::fill(cells_.begin(), cells_.begin() + song_channels_, Cell());
        for (int c = 0; c < music_; c++)
        {
            Update(channels_[size_t(c)]);
        }
    }
    else
    {
        uint32_t bits = info_.bank.address + offset;
        uint32_t values = bits + row_bytes_;
        uint8_t bit = 0x80;
        const auto field = [&]()
        {
            uint8_t value = 0;
            if (rom_.U8(bits) & bit)
            {
                value = rom_.U8(values++);
            }

            if (bit > 1)
            {
                bit = uint8_t(bit >> 1);
            }
            else
            {
                bit = 0x80;
                bits++;
            }

            return value;
        };

        for (int c = 0; c < song_channels_; c++)
        {
            Cell& cell = cells_[size_t(c)];
            cell.note = field();
            cell.instrument = field();
            cell.volume = field();
            cell.effect = field();
            cell.operand = field();
            if (c < music_)
            {
                Channel& ch = channels_[size_t(c)];
                StartInstrument(ch, cell);
                StartNote(ch, cell);
                VolumeColumn(ch, cell);
                RowEffect(ch, cell);
                Update(ch);
            }
        }
    }

    if (row_ >= rom_.U16(pattern_))
    {
        NextOrder(0);
    }
}

uint32_t Player::Instrument(uint8_t number)
{
    if (number == 0 || number > info_.bank.instruments.size())
    {
        unmodelled_++;
        return 0;
    }

    return info_.bank.instruments[number - 1u];
}

// A row's instrument (0x08001E54). An instrument sets the channel's volume, pan and relative note. Without a note, it
// starts the envelopes and the fade-out again. With a note and no slide to it, it starts its sample. A note without an
// instrument and without an effect starts the channel's sample again.
void Player::StartInstrument(Channel& c, const Cell& cell)
{
    if (cell.instrument != 0)
    {
        const uint32_t inst = Instrument(cell.instrument);
        c.instrument = inst;
        c.volume = int8_t(rom_.U8(inst + kInstrumentVolume));
        c.pan = InstrumentPan(rom_.U8(inst + kInstrumentPan));
        c.flags |= kChannelVolume;
        c.relative = int8_t(rom_.U8(inst + kInstrumentRelativeNote));
        if (cell.note == 0)
        {
            c.fade = 0xFFFF;
            ResetEnvelopes(c);
            return;
        }

        if (cell.effect == kEffectSlideToNote || cell.effect == kEffectSlideToNoteVolume)
        {
            return;
        }

        c.loop_bytes = rom_.U32(inst + kInstrumentLoopBytes);
        const uint32_t end = c.loop_bytes != 0 ? rom_.U32(inst + kInstrumentLoopStart) + c.loop_bytes
                                               : rom_.U32(inst + kInstrumentLength);
        c.end = inst + kInstrumentSamples + end;
        c.fine_tune = int8_t(rom_.U8(inst + kInstrumentFineTune));
        c.place = inst + kInstrumentSamples;
        c.fraction = 0;
        c.starts++;
        c.sample = c.place;
        c.sample_instrument = inst;
        return;
    }

    if (cell.note != 0 && cell.note != kNoteKeyOff && cell.effect == 0)
    {
        c.place = c.instrument + kInstrumentSamples;
        c.fraction = 0;
        c.starts++;
        c.sample = c.place;
        c.sample_instrument = c.instrument;
    }
}

void Player::ResetEnvelopes(Channel& c)
{
    c.volume_envelope = {.value = 0x40};
    c.pan_envelope = {.value = 0x20};
}

// A key off: the release where the instrument has a volume envelope, else the channel stops.
void Player::Release(Channel& c)
{
    if (rom_.U8(c.instrument + kInstrumentVolumeEnvelope + kEnvelopeCount) != 0)
    {
        c.flags |= kChannelReleased;
    }
    else
    {
        c.flags = kChannelOff;
    }
}

// A row's note (0x08001F00): a key off, a slide's target, or a new note that starts the envelopes and turns the
// channel on.
void Player::StartNote(Channel& c, const Cell& cell)
{
    if (cell.note == 0)
    {
        return;
    }

    if (cell.note == kNoteKeyOff)
    {
        Release(c);
        return;
    }

    if (cell.effect == kEffectSlideToNote || cell.effect == kEffectSlideToNoteVolume)
    {
        c.target = cell.note;
        return;
    }

    c.note = cell.note;
    c.base = cell.note;
    c.slide = 0;
    c.vibrato_place = 0;
    c.fade = 0xFFFF;
    ResetEnvelopes(c);
    c.flags = uint16_t((c.flags & ~(kChannelOff | kChannelReleased)) | kChannelPitch | kChannelVolume);
}

// A row's volume column (0x08001F70): a volume, slides of the volume by 4 x or 1 x the low digit, a pan, and moves of
// the pan. The slides and moves act once, at the row.
void Player::VolumeColumn(Channel& c, const Cell& cell)
{
    const uint16_t v = cell.volume;
    if (v == 0)
    {
        return;
    }

    const auto in = [&](uint16_t first, uint16_t span)
    {
        return uint16_t(v - first) <= span;
    };

    if (in(kColumnVolume, 0x40))
    {
        c.volume = int8_t(v - kColumnVolume);
    }
    else if (in(0x61, kColumnFine))
    {
        c.volume = int8_t(uint8_t(c.volume) - 4 * (v - 0x60));
        c.volume = std::max<int8_t>(c.volume, 0);
    }
    else if (in(0x71, kColumnFine))
    {
        c.volume = int8_t(uint8_t(c.volume) + 4 * (v - 0x70));
        c.volume = std::min<int8_t>(c.volume, 0x40);
    }
    else if (in(0x81, kColumnFine))
    {
        c.volume = int8_t(uint8_t(c.volume) - (v - 0x80));
        c.volume = std::max<int8_t>(c.volume, 0);
    }
    else if (in(0x91, kColumnFine))
    {
        c.volume = int8_t(uint8_t(c.volume) + (v - 0x90));
        c.volume = std::min<int8_t>(c.volume, 0x40);
    }
    else if (in(0xC0, 0x0F))
    {
        c.pan = int8_t(uint8_t(8 * v + 0x1C0));
    }
    else if (in(0xD1, kColumnFine))
    {
        c.pan = int8_t(uint8_t(c.pan) - (v - 0xD0));
        c.pan = std::max<int8_t>(c.pan, -0x40);
    }
    else if (in(0xE1, kColumnFine))
    {
        c.pan = int8_t(uint8_t(c.pan) + (v - 0xE0));
        c.pan = std::min<int8_t>(c.pan, 0x40);
    }
    else
    {
        return;
    }

    c.flags |= kChannelVolume;
}

// A row's effect (0x0800203C, jump table 0x08002058). The effects without a handler call the game's callback. The
// build sets no callback.
void Player::RowEffect(Channel& c, const Cell& cell)
{
    const uint16_t op = cell.operand;
    switch (cell.effect)
    {
    case kEffectSlideUp:
    case kEffectSlideDown:
    case kEffectSlideToNote:
        if (op != 0)
        {
            c.slide_speed = uint16_t(op << 3);
        }
        break;

    case kEffectVibrato:
        if (op >> 4)
        {
            c.vibrato_speed = uint8_t(op >> 4);
        }
        if (op & 0x0F)
        {
            c.vibrato_depth = uint8_t(op & 0x0F);
        }
        break;

    case kEffectSlideToNoteVolume:
    case kEffectVibratoVolume:
    case kEffectVolumeSlide:
        if (op != 0)
        {
            c.volume_slide = int8_t(op <= 0x0F ? -int(op) : int(op >> 4));
        }
        break;

    case kEffectPan:
        c.pan = int8_t((op >> 1) - 0x40);
        c.flags |= kChannelVolume;
        break;

    case kEffectOffset:
        // The offset moves the place on from the sample's start, and back by the loop's bytes while it's past the
        // end. A sample that plays once never comes back: the driver's loop doesn't end.
        if (cell.instrument != 0 && op != 0)
        {
            c.place += uint32_t(op) << 8;
            while (c.place > c.end)
            {
                if (c.loop_bytes == 0)
                {
                    unmodelled_++;
                    break;
                }

                c.place -= c.loop_bytes;
            }
        }
        break;

    case kEffectJump:
        order_ = uint8_t((op & 0xFF) - 1);
        row_ = rom_.U16(pattern_);
        break;

    case kEffectVolume:
        c.volume = int8_t(op);
        c.flags |= kChannelVolume;
        break;

    case kEffectBreak:
        NextOrder(uint16_t((op >> 4) * 10 + (op & 0x0F)));
        break;

    case kEffectExtended:
        switch (op >> 4)
        {
        case kExtendedFineTune:
            c.fine_tune = int16_t(op & 0x0F);
            break;

        case kExtendedFineVolumeUp:
            c.volume = std::min<int8_t>(int8_t(uint8_t(c.volume) + (op & 0x0F)), 0x40);
            c.flags |= kChannelVolume;
            break;

        case kExtendedFineVolumeDown:
            c.volume = std::max<int8_t>(int8_t(uint8_t(c.volume) - (op & 0x0F)), 0);
            c.flags |= kChannelVolume;
            break;

        case kExtendedNoteDelay:
            c.delay = uint8_t(op & 0x0F);
            if (c.delay != 0)
            {
                c.flags |= kChannelOff;
                c.delay--;
            }
            break;

        case kExtendedPatternDelay:
            pattern_delay_ = uint8_t(op & 0x0F);
            break;

        default:
            break;
        }
        break;

    case kEffectSpeed:
        if (op <= 0x0F)
        {
            speed_ = uint8_t(op);
        }
        else
        {
            tempo_ = uint8_t(op);
            tick_samples_ = uint16_t(engine_software::TickSamples(StoredRate(info_.build.rate), tempo_));
        }
        break;

    case kEffectMusicVolume:
        SetMusicVolume(op);
        break;

    case kEffectMusicVolumeSlide:
        {
            // The slide works on the music volume alone. The channels' levels follow it at their next change.
            const int delta = op <= 0x0F ? -int(op) : int(op >> 4);
            music_volume_ = uint16_t(music_volume_ + delta);
            if (music_volume_ > 0x100)
            {
                music_volume_ = delta >= 0 ? 0x100 : 0;
            }
            break;
        }

    case kEffectKeyOff:
        Release(c);
        break;

    case kEffectEnvelopePlace:
        c.pan_envelope.tick = op;
        c.volume_envelope.tick = op;
        break;

    case kEffectRetrigger:
        c.retrigger = uint8_t(op & 0x0F);
        if (c.retrigger != 0)
        {
            c.retrigger--;
        }

        c.place = c.instrument + kInstrumentSamples;
        c.fraction = 0;
        c.starts++;
        c.sample = c.place;
        c.sample_instrument = c.instrument;
        break;

    default:
        break;
    }
}

// An effect on a tick after the row (0x08001B1C, jump table 0x08001B50), then the volume and pitch.
void Player::TickEffect(Channel& c, const Cell& cell)
{
    const uint16_t op = cell.operand;
    switch (cell.effect)
    {
    case kEffectArpeggio:
        if (op != 0)
        {
            const int phase = tick_ % 3;
            c.note = uint8_t(c.base + (phase == 0 ? 0 : phase == 1 ? (op >> 4) : (op & 0x0F)));
            c.flags |= kChannelPitch;
        }
        break;

    case kEffectSlideUp:
        c.slide = int16_t(c.slide + c.slide_speed);
        c.flags |= kChannelPitch;
        break;

    case kEffectSlideDown:
        c.slide = int16_t(c.slide - c.slide_speed);
        c.flags |= kChannelPitch;
        break;

    case kEffectSlideToNoteVolume:
        c.volume = SlideVolume(c.volume, c.volume_slide);
        c.flags |= kChannelVolume;
        [[fallthrough]];

    case kEffectSlideToNote:
        if (c.slide_speed != 0 && c.target != c.base)
        {
            const int32_t difference = c.target * kNoteSteps - (c.base * kNoteSteps + c.slide);
            if (uint32_t(difference < 0 ? -difference : difference) > c.slide_speed)
            {
                c.slide = int16_t(difference > 0 ? c.slide + c.slide_speed : c.slide - c.slide_speed);
            }
            else
            {
                c.slide = 0;
                c.note = c.target;
                c.base = c.target;
            }

            c.flags |= kChannelPitch;
        }
        break;

    case kEffectVibrato:
        Vibrato(c);
        c.flags |= kChannelPitch;
        break;

    case kEffectVibratoVolume:
        Vibrato(c);
        c.volume = SlideVolume(c.volume, c.volume_slide);
        c.flags |= kChannelPitch | kChannelVolume;
        break;

    case kEffectTremolo:
        Tremolo(c);
        c.flags |= kChannelVolume;
        break;

    case kEffectVolumeSlide:
        c.volume = SlideVolume(c.volume, c.volume_slide);
        c.flags |= kChannelVolume;
        break;

    case kEffectExtended:
        if ((op >> 4) == kExtendedNoteDelay && c.delay != 0)
        {
            c.delay--;
            if (c.delay == 0)
            {
                c.flags &= uint16_t(~kChannelOff);
            }
        }
        break;

    case kEffectRetrigger:
        if (c.retrigger == 0)
        {
            c.place = c.instrument + kInstrumentSamples;
            c.fraction = 0;
            c.starts++;
            c.sample = c.place;
            c.sample_instrument = c.instrument;
            c.retrigger = uint8_t(op & 0x0F);
            if (c.retrigger == 0)
            {
                break;
            }
        }

        c.retrigger--;
        break;

    default:
        break;
    }

    Update(c);
}

const std::vector<int8_t>& Player::Output() const
{
    return output_;
}

bool Player::Playing() const
{
    return (flags_ & (kSongPlays | kSongEnded)) == kSongPlays;
}

uint8_t Player::Order() const
{
    return order_;
}

uint8_t Player::Passes() const
{
    return passes_;
}

uint16_t Player::Row() const
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

uint8_t Player::Tempo() const
{
    return tempo_;
}

uint8_t Player::PatternDelay() const
{
    return pattern_delay_;
}

uint16_t Player::MusicVolume() const
{
    return music_volume_;
}

uint16_t Player::TickSamples() const
{
    return tick_samples_;
}

int Player::MusicChannels() const
{
    return music_;
}

const Channel& Player::ChannelAt(int c) const
{
    return channels_[size_t(c)];
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

uint16_t Player::RowNumber() const
{
    return row_number_;
}

int Player::Unmodelled() const
{
    return unmodelled_;
}

} // namespace agblib::engine_software
