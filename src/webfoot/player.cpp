// SPDX-License-Identifier: MIT

#include "agblib/webfoot/player.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/webfoot/driver.h"
#include "player_internal.h"

namespace agblib::webfoot
{

using agblib::agb::Hex;

namespace
{

// The effects of the driver's table (0x08166734) that do more than start the row's note: Impulse Tracker's letters,
// and effect 27, a delay of the note.
enum EffectNumber : uint8_t
{
    kSetSpeed = 1,       // A
    kJump = 2,           // B
    kVolumeSlide = 4,    // D
    kSlideDown = 5,      // E
    kSlideUp = 6,        // F
    kPorta = 7,          // G
    kVibrato = 8,        // H
    kVibratoVolume = 11, // K
    kPortaVolume = 12,   // L
    kOffset = 15,        // O
    kRetrigger = 17,     // Q
    kSetTempo = 20,      // T
    kFineVibrato = 21,   // U
    kPan = 24,           // X
    kNoteDelay = 27,
};

// The full level of a volume.
constexpr int kFullVolume = 64;

} // namespace

Player::Player(const Rom& rom, const DriverInfo& info, int song, int music_volume)
    : rom_(rom), music_volume_(std::clamp(music_volume, 0, 255))
{
    int index = 0;
    const SongTable* table = info.TableOf(song, index);
    if (table == nullptr)
    {
        Warn("no song " + std::to_string(song));
        return;
    }

    tables_ = table->tables;
    build_ = table->build;
    mixer_ = MixerOf(table->build);
    header_ = table->songs[size_t(index)];
    mix_samples_ = table->mix_samples;
    if (mix_samples_ == 0)
    {
        Warn("the song's table has no mix routine");
        return;
    }

    if (header_.tempo == 0)
    {
        Warn("the song's tempo is 0");
        return;
    }

    // The play routine (0x0800A5F6) and the reset of the channels (0x0800A5C0).
    row_ = 0;
    rows_ = 1;
    tick_counter_ = 1;
    speed_ = header_.speed;
    tempo_ = header_.tempo;
    order_ = -1;
    tick_samples_ = mixer_.tick_scale / tempo_;
    samples_left_ = 0;
    valid_ = true;
    playing_ = true;
}

bool Player::Valid() const
{
    return valid_;
}

const SongHeader& Player::Header() const
{
    return header_;
}

const Mixer& Player::MixerRules() const
{
    return mixer_;
}

void Player::OnStep(std::function<void(bool)> callback)
{
    on_step_ = std::move(callback);
}

uint64_t Player::Elapsed() const
{
    return elapsed_;
}

int Player::RowOrder() const
{
    return row_order_;
}

int Player::RowNumber() const
{
    return row_number_;
}

uint32_t Player::RowSamples() const
{
    return row_samples_;
}

uint32_t Player::Speed() const
{
    return speed_;
}

uint32_t Player::Tempo() const
{
    return tempo_;
}

std::vector<uint32_t> Player::RepeatState() const
{
    return {speed_, tempo_};
}

const std::array<Channel, kChannels>& Player::Channels() const
{
    return channels_;
}

const std::array<Voice, kChannels>& Player::Voices() const
{
    return voices_;
}

bool Player::Playing() const
{
    return playing_;
}

const std::vector<std::string>& Player::Warnings() const
{
    return warnings_;
}

void Player::Warn(const std::string& text)
{
    if (std::find(warnings_.begin(), warnings_.end(), text) == warnings_.end())
    {
        warnings_.push_back(text);
    }
}

uint32_t Player::StepOf(uint8_t note) const
{
    return rom_.U32(tables_.steps + 4 * uint32_t(note));
}

uint16_t Player::SlideUp(uint32_t index) const
{
    return rom_.U16(tables_.slide_up + 2 * index);
}

uint16_t Player::SlideDown(uint32_t index) const
{
    return rom_.U16(tables_.slide_down + 2 * index);
}

uint16_t Player::FineUp(uint32_t index) const
{
    return rom_.U16(tables_.slide_up - kFineSlideBytes + 2 * index);
}

uint16_t Player::FineDown(uint32_t index) const
{
    return rom_.U16(tables_.slide_down - kFineSlideBytes + 2 * index);
}

void Player::Tick()
{
    // The tick routine (0x0800A4C6).
    row_read_ = false;
    tick_counter_ = uint8_t(tick_counter_ - 1);
    if (tick_counter_ == 0)
    {
        // The row's place before its effects: a jump changes the order and the row that the driver reads next.
        ReadRow();
        if (!playing_)
        {
            return;
        }

        row_order_ = order_;
        row_number_ = row_;
        for (int c = 0; c < kChannels; c++)
        {
            RowEffect(c);
            if (!playing_)
            {
                return;
            }
        }

        tick_counter_ = speed_;
        row_read_ = true;
        row_samples_ = uint32_t(speed_) * tick_samples_;
    }
    else
    {
        for (int c = 0; c < kChannels; c++)
        {
            const Channel& ch = channels_[size_t(c)];
            const uint8_t mode = ch.flags & kChannelTickMode;
            if (mode == 0 || (mode == 1 && !(ch.flags & kChannelSounding)))
            {
                continue;
            }

            RunTickRoutine(c);
        }
    }

    for (int c = 0; c < kChannels; c++)
    {
        UpdateVoice(c);
    }
}

void Player::ReadRow()
{
    // The row reader (0x0800A3E6).
    row_ = uint8_t(row_ + 1);
    if (row_ >= rows_)
    {
        order_ = int16_t(order_ + 1);
        if (rom_.U8(header_.orders + uint32_t(int32_t(order_))) == kOrderEnd)
        {
            order_ = 0;
        }

        const uint8_t pattern_number = rom_.U8(header_.orders + uint32_t(int32_t(order_)));
        const uint32_t pattern = rom_.U32(header_.patterns + 4 * uint32_t(pattern_number));
        if (!rom_.Contains(pattern))
        {
            Warn("order " + std::to_string(order_) + " names a pattern outside the ROM");
            playing_ = false;
            return;
        }

        row_ = 0;
        rows_ = rom_.U8(pattern);
        row_address_ = pattern + 1;
    }

    for (Channel& ch : channels_)
    {
        ch.mask = 0;
    }

    const auto next = [this]()
    {
        return rom_.U8(row_address_++);
    };
    while (true)
    {
        const uint8_t b = next();
        if (b == 0)
        {
            break;
        }

        const int c = (b & 0x0F) - 1;
        if (c < 0)
        {
            // The driver writes the entry into the memory before its first channel's record.
            Warn("a row at " + Hex(row_address_ - 1) + " has an entry for channel 0");
            playing_ = false;
            return;
        }

        Channel& ch = channels_[size_t(c)];
        const uint8_t mode = b >> 4;
        if (mode == 0)
        {
            ch.mask = next();
            ch.last_mask = ch.mask;
        }
        else if (mode == 1)
        {
            ch.mask = ch.last_mask;
        }
        else
        {
            ch.mask = rom_.U8(header_.masks + mode - 2);
        }

        if (ch.mask & kMaskNote)
        {
            ch.note = next();
        }

        if (ch.mask & kMaskInstrument)
        {
            ch.instrument = next();
        }

        if (ch.mask & kMaskVolume)
        {
            ch.volume_byte = next();
        }

        if (ch.mask & kMaskEffect)
        {
            ch.effect = next();
            ch.parameter = next();
        }
    }
}

void Player::SetPitch(int c, uint32_t step)
{
    Voice& v = voices_[size_t(c)];
    v.pitch = step;
    v.flags |= kVoicePitch;
}

void Player::SetVolume(int c)
{
    Voice& v = voices_[size_t(c)];
    v.volume = uint8_t(channels_[size_t(c)].volume);
    v.flags |= kVoiceLevels;
}

void Player::RowEffect(int c)
{
    // The routine that starts a row on a channel (0x0800A3A6). A vibrato's pitch lasts until the next row.
    Channel& ch = channels_[size_t(c)];
    Voice& v = voices_[size_t(c)];
    ch.flags &= uint8_t(~kChannelTickMode);
    if ((ch.flags & kChannelSounding) && v.pitch != ch.step)
    {
        SetPitch(c, ch.step);
    }

    if (ch.mask != 0)
    {
        Effect(c, (ch.mask & (kMaskEffect | kMaskLastEffect)) ? ch.effect : 0);
    }
}

void Player::Note(int c)
{
    // The routine that starts the row's note (0x08009A20). Most effects run it first.
    Channel& ch = channels_[size_t(c)];
    Voice& v = voices_[size_t(c)];
    if (ch.mask & (kMaskNote | kMaskLastNote))
    {
        if (ch.note == kNoteOff)
        {
            v.flags &= uint8_t(~kVoiceOn);
            ch.flags &= uint8_t(~kChannelSounding);
            return;
        }

        ch.volume = int8_t(ch.volume_byte);
        ch.step = StepOf(ch.note);
        ch.flags = kChannelSounding;
        v.pitch = ch.step;
        v.volume = uint8_t(ch.volume);
        v.starts++;
        if (ch.mask & kMaskLastInstrument)
        {
            v.flags |= kVoiceOn | kVoicePitch | kVoiceRestart | kVoiceLevels;
        }
        else
        {
            v.sample = header_.samples + kSampleSize * ch.instrument;
            v.flags |= kVoiceOn | kVoicePitch | kVoiceRestart | kVoiceSample | kVoiceLevels;
        }
    }
    else if (ch.mask & (kMaskVolume | kMaskLastVolume))
    {
        ch.volume = int8_t(ch.volume_byte);
        SetVolume(c);
    }
}

void Player::VolumeDown(int c)
{
    // 0x08009AD2
    Channel& ch = channels_[size_t(c)];
    ch.volume = int8_t(ch.volume - ch.slide);
    if (ch.volume <= 0)
    {
        ch.volume = 0;
        ch.flags &= uint8_t(~kChannelTickMode);
    }

    SetVolume(c);
}

void Player::VolumeUp(int c)
{
    // 0x08009AFE
    Channel& ch = channels_[size_t(c)];
    ch.volume = int8_t(ch.volume + ch.slide);
    if (ch.volume >= kFullVolume)
    {
        ch.volume = kFullVolume;
        ch.flags &= uint8_t(~kChannelTickMode);
    }

    SetVolume(c);
}

void Player::VolumePart(int c, bool up)
{
    // The volume slide of a slide to a note with a volume slide (0x0800A1C6 and 0x0800A2D0). Once the volume stops,
    // the channel's tick routine becomes the slide down to the note, whatever the slide's way.
    Channel& ch = channels_[size_t(c)];
    if (up)
    {
        ch.volume = int8_t(ch.volume + ch.slide);
        if (ch.volume >= kFullVolume)
        {
            ch.volume = kFullVolume;
            ch.routine = TickRoutine::kPortaDown;
        }
    }
    else
    {
        ch.volume = int8_t(ch.volume - ch.slide);
        if (ch.volume <= 0)
        {
            ch.volume = 0;
            ch.routine = TickRoutine::kPortaDown;
        }
    }

    SetVolume(c);
}

void Player::Vibrato(int c)
{
    // 0x08009DB0: the vibrato moves the voice's pitch, and leaves the channel's step.
    Channel& ch = channels_[size_t(c)];
    ch.vibrato_place = uint8_t((ch.vibrato_place + ch.vibrato_speed) & 63);
    const int32_t depth = int32_t(rom_.S8(tables_.vibrato + ch.vibrato_place)) * ch.vibrato_depth >> 6;
    if (depth < 0)
    {
        SetPitch(c, Mul(ch.step, FineDown(uint32_t(-depth))) >> 16);
    }
    else
    {
        SetPitch(c, Mul(ch.step, FineUp(uint32_t(depth))) >> 15);
    }
}

void Player::SlideBy(int c, uint32_t factor, int shift)
{
    Channel& ch = channels_[size_t(c)];
    ch.step = Mul(ch.step, factor) >> shift;
    SetPitch(c, ch.step);
}

void Player::PortaClamp(int c, TickRoutine then, bool volume_up)
{
    // A slide to a note with a volume slide that passes its target: the step stops there, and the volume slide runs on.
    Channel& ch = channels_[size_t(c)];
    ch.step = ch.target;
    SetPitch(c, ch.step);
    ch.flags &= uint8_t(~kChannelTarget);
    ch.routine = then;
    if (volume_up)
    {
        VolumeUp(c);
    }
    else
    {
        VolumeDown(c);
    }
}

void Player::RunTickRoutine(int c)
{
    Channel& ch = channels_[size_t(c)];
    Voice& v = voices_[size_t(c)];
    switch (ch.routine)
    {
    case TickRoutine::kNone:
        break;
    case TickRoutine::kVolumeDown:
        VolumeDown(c);
        break;
    case TickRoutine::kVolumeUp:
        VolumeUp(c);
        break;
    case TickRoutine::kSlideDown:
        SlideBy(c, SlideDown(ch.pitch_memory), 16);
        break;
    case TickRoutine::kSlideUp:
        SlideBy(c, SlideUp(ch.pitch_memory), 14);
        break;
    case TickRoutine::kPortaDown:
        SlideBy(c, SlideDown(ch.pitch_memory), 16);
        if (ch.step < ch.target)
        {
            ch.step = ch.target;
            SetPitch(c, ch.step);
            ch.flags &= uint8_t(~(kChannelTarget | kChannelTickMode));
        }
        break;
    case TickRoutine::kPortaUp:
        SlideBy(c, SlideUp(ch.pitch_memory), 14);
        if (ch.step > ch.target)
        {
            ch.step = ch.target;
            SetPitch(c, ch.step);
            ch.flags &= uint8_t(~(kChannelTarget | kChannelTickMode));
        }
        break;
    case TickRoutine::kVibrato:
        Vibrato(c);
        break;
    case TickRoutine::kVibratoVolumeDown:
        // 0x08009E7E
        ch.volume = int8_t(ch.volume - ch.slide);
        if (ch.volume <= 0)
        {
            ch.volume = 0;
            ch.routine = TickRoutine::kVibrato;
        }

        SetVolume(c);
        Vibrato(c);
        break;
    case TickRoutine::kVibratoVolumeUp:
        // 0x08009EAC
        ch.volume = int8_t(ch.volume + ch.slide);
        if (ch.volume >= kFullVolume)
        {
            ch.volume = kFullVolume;
            ch.routine = TickRoutine::kVibrato;
        }

        SetVolume(c);
        Vibrato(c);
        break;
    case TickRoutine::kRetrigger:
        // 0x0800A092: in the 2002 build, the note starts again once, and the routine stops. The 2003 build keeps the
        // routine, and the note starts again every count of ticks until the next row (0x080204D4 in The Legacy of
        // Goku II).
        ch.counter = uint8_t(ch.counter - 1);
        if (ch.counter == 0)
        {
            v.flags |= kVoiceOn | kVoiceRestart;
            v.starts++;
            ch.counter = ch.retrigger_memory & 0x0F;
            ch.flags = build_ == Build::k2002 ? kChannelSounding : uint8_t(ch.flags | kChannelSounding);
        }
        break;
    case TickRoutine::kNoteDelay:
        // 0x0800A0E4
        ch.counter = uint8_t(ch.counter - 1);
        if (ch.counter == 0)
        {
            Note(c);
        }
        break;
    case TickRoutine::kPortaUpVolumeUp:
        // 0x0800A182. The comparison is the other way round from the slide's others: the step jumps to its target at
        // the first tick that leaves it below, and slides on above it.
        SlideBy(c, SlideUp(ch.pitch_memory), 14);
        if (ch.step >= ch.target)
        {
            VolumePart(c, true);
        }
        else
        {
            PortaClamp(c, TickRoutine::kVolumeUp, true);
        }
        break;
    case TickRoutine::kPortaDownVolumeUp:
        // 0x0800A1EE
        SlideBy(c, SlideDown(ch.pitch_memory), 16);
        if (ch.step >= ch.target)
        {
            VolumePart(c, true);
        }
        else
        {
            PortaClamp(c, TickRoutine::kVolumeUp, true);
        }
        break;
    case TickRoutine::kPortaUpVolumeDown:
        // 0x0800A28C
        SlideBy(c, SlideUp(ch.pitch_memory), 14);
        if (ch.step <= ch.target)
        {
            VolumePart(c, false);
        }
        else
        {
            PortaClamp(c, TickRoutine::kVolumeDown, false);
        }
        break;
    case TickRoutine::kPortaDownVolumeDown:
        // 0x0800A2F8
        SlideBy(c, SlideDown(ch.pitch_memory), 16);
        if (ch.step >= ch.target)
        {
            VolumePart(c, false);
        }
        else
        {
            PortaClamp(c, TickRoutine::kVolumeDown, false);
        }
        break;
    }
}

void Player::VolumeSlideSetting(int c, TickRoutine up, TickRoutine down, TickRoutine level)
{
    // The decoding of D's and K's parameter (0x08009B3C and 0x08009F00): x0 slides up and xF up once, 0x slides down
    // and Fx down once. A slide of 15 also runs once at once. `level` is the routine that a slide once leaves behind,
    // or kNone to leave the routine as it is.
    Channel& ch = channels_[size_t(c)];
    const uint8_t x = ch.volume_memory;
    const uint8_t low = x & 0x0F;
    const uint8_t high = x >> 4;
    if (low == 0)
    {
        ch.slide = high;
        ch.routine = up;
        if (level == TickRoutine::kNone)
        {
            ch.flags |= 1;
        }

        if (ch.slide == 15)
        {
            VolumeUp(c);
        }
    }
    else if (high == 0)
    {
        ch.slide = x;
        ch.routine = down;
        if (level == TickRoutine::kNone)
        {
            ch.flags |= 1;
        }

        if (ch.slide == 15)
        {
            VolumeDown(c);
        }
    }
    else if (low == 0x0F)
    {
        ch.slide = high;
        VolumeUp(c);
        if (level != TickRoutine::kNone)
        {
            ch.routine = level;
        }
    }
    else if (high == 0x0F)
    {
        ch.slide = low;
        VolumeDown(c);
        if (level != TickRoutine::kNone)
        {
            ch.routine = level;
        }
    }
}

void Player::Effect(int c, uint8_t effect)
{
    Channel& ch = channels_[size_t(c)];
    Voice& v = voices_[size_t(c)];
    const uint8_t p = ch.parameter;
    if (effect >= kEffects)
    {
        Warn("effect " + std::to_string(effect) + " is past the driver's table");
        playing_ = false;
        return;
    }

    switch (effect)
    {
    case kSetSpeed:
        Note(c);
        speed_ = p;
        break;
    case kJump:
        // The next row starts order p.
        Note(c);
        row_ = uint8_t(rows_ - 1);
        order_ = int16_t(p - 1);
        break;
    case kVolumeSlide:
        // 0x08009B2A
        Note(c);
        if (p != 0)
        {
            ch.volume_memory = p;
        }

        VolumeSlideSetting(c, TickRoutine::kVolumeUp, TickRoutine::kVolumeDown, TickRoutine::kNone);
        break;
    case kSlideDown:
    case kSlideUp:
        {
            // 0x08009BC4 and 0x08009C52: Fx slides by a fine step once, Ex by an extra fine step once, and the rest at
            // each tick.
            Note(c);
            if (p != 0)
            {
                ch.pitch_memory = p;
            }

            if (!(ch.flags & kChannelSounding))
            {
                break;
            }

            const uint8_t x = ch.pitch_memory;
            const bool down = effect == kSlideDown;
            if ((x >> 4) == 0x0F)
            {
                SlideBy(c, down ? SlideDown(x & 0x0F) : SlideUp(x & 0x0F), down ? 16 : 14);
            }
            else if ((x >> 4) == 0x0E)
            {
                SlideBy(c, down ? FineDown(x & 0x0F) : FineUp(x & 0x0F), down ? 16 : 15);
            }
            else
            {
                ch.routine = down ? TickRoutine::kSlideDown : TickRoutine::kSlideUp;
                ch.flags |= 1;
            }
            break;
        }
    case kPorta:
        {
            // 0x08009D3E: no new note, and the volume byte still sets the volume.
            if (p != 0)
            {
                ch.pitch_memory = p;
            }

            if (!(ch.flags & kChannelSounding))
            {
                break;
            }

            bool slides = false;
            if (ch.mask & (kMaskNote | kMaskLastNote))
            {
                if (ch.note != kNoteOff)
                {
                    ch.target = StepOf(ch.note);
                    ch.flags |= kChannelTarget;
                    slides = true;
                }
            }
            else
            {
                slides = (ch.flags & kChannelTarget) != 0;
            }

            if (slides)
            {
                ch.routine = ch.target > ch.step ? TickRoutine::kPortaUp : TickRoutine::kPortaDown;
                ch.flags |= 1;
            }

            if (ch.mask & (kMaskVolume | kMaskLastVolume))
            {
                ch.volume = int8_t(ch.volume_byte);
                SetVolume(c);
            }
            break;
        }
    case kVibrato:
    case kFineVibrato:
        // 0x08009DF4 and 0x0800A120. H's depth is 4 times U's, and only H waits for a sounding note.
        Note(c);
        if (effect == kVibrato && !(ch.flags & kChannelSounding))
        {
            break;
        }

        if (ch.mask & (kMaskNote | kMaskLastNote))
        {
            ch.vibrato_place = 0;
        }

        if (p & 0x0F)
        {
            ch.vibrato_depth = uint8_t((p & 0x0F) * (effect == kVibrato ? 4 : 1));
        }

        if (p >> 4)
        {
            ch.vibrato_speed = p >> 4;
        }

        ch.routine = TickRoutine::kVibrato;
        ch.flags |= 1;
        Vibrato(c);
        break;
    case kVibratoVolume:
        // 0x08009EDA
        Note(c);
        if (p != 0)
        {
            ch.volume_memory = p;
        }

        if (!(ch.flags & kChannelSounding))
        {
            break;
        }

        if (ch.mask & (kMaskNote | kMaskLastNote))
        {
            ch.vibrato_place = 0;
        }

        VolumeSlideSetting(c, TickRoutine::kVibratoVolumeUp, TickRoutine::kVibratoVolumeDown, TickRoutine::kVibrato);
        ch.flags |= 1;
        Vibrato(c);
        break;
    case kPortaVolume:
        {
            // 0x08009F70: no new note. The volume byte sets the volume first.
            if (p != 0)
            {
                ch.volume_memory = p;
            }

            if (!(ch.flags & kChannelSounding))
            {
                break;
            }

            if (ch.mask & (kMaskVolume | kMaskLastVolume))
            {
                ch.volume = int8_t(ch.volume_byte);
                SetVolume(c);
            }

            int slide = 0; // 1: up at each tick, 2: down at each tick
            const uint8_t x = ch.volume_memory;
            const uint8_t low = x & 0x0F;
            const uint8_t high = x >> 4;
            if (low == 0)
            {
                ch.slide = high;
                slide = 1;
                if (ch.slide == 15)
                {
                    VolumeUp(c);
                }
            }
            else if (high == 0)
            {
                ch.slide = x;
                slide = 2;
                if (ch.slide == 15)
                {
                    VolumeDown(c);
                }
            }
            else if (low == 0x0F)
            {
                ch.slide = high;
                VolumeUp(c);
            }
            else if (high == 0x0F)
            {
                ch.slide = low;
                VolumeDown(c);
            }

            if (ch.mask & (kMaskNote | kMaskLastNote))
            {
                if (ch.note == kNoteOff)
                {
                    break;
                }

                ch.target = StepOf(ch.note);
                ch.flags |= kChannelTarget;
            }
            else if (!(ch.flags & kChannelTarget) && slide != 0)
            {
                ch.routine = slide == 1 ? TickRoutine::kVolumeUp : TickRoutine::kVolumeDown;
                ch.flags |= 1;
            }

            // The slide's routine replaces any other, toward the last target even without one on this row.
            static constexpr TickRoutine kUp[] = {TickRoutine::kPortaUp, TickRoutine::kPortaUpVolumeUp,
                                                  TickRoutine::kPortaUpVolumeDown};
            static constexpr TickRoutine kDown[] = {TickRoutine::kPortaDown, TickRoutine::kPortaDownVolumeUp,
                                                    TickRoutine::kPortaDownVolumeDown};
            ch.routine = ch.target > ch.step ? kUp[slide] : kDown[slide];
            ch.flags |= 1;
            break;
        }
    case kOffset:
        // 0x0800A06C: the note starts p * 256 points into its sample. The 2002 build leaves the way of a ping-pong loop
        // as it was. The 2003 build plays forward again (0x080204CA in The Legacy of Goku II).
        Note(c);
        if (ch.mask & (kMaskNote | kMaskLastNote))
        {
            v.position = int32_t(uint32_t(p) << 22);
            v.flags &= uint8_t(~kVoiceRestart);
            if (build_ == Build::k2003)
            {
                v.sample_flags &= uint8_t(~kVoiceBackwards);
            }
        }
        break;
    case kRetrigger:
        // 0x0800A0B6
        Note(c);
        if (p != 0)
        {
            ch.retrigger_memory = p;
        }

        ch.routine = TickRoutine::kRetrigger;
        ch.counter = ch.retrigger_memory & 0x0F;
        ch.flags = uint8_t((ch.flags & ~kChannelTickMode) | 2);
        break;
    case kSetTempo:
        // 0x0800A10E: the next tick lasts the new tempo's samples.
        Note(c);
        tempo_ = p;
        if (p == 0)
        {
            Warn("a row sets the tempo to 0");
            playing_ = false;
            break;
        }

        tick_samples_ = mixer_.tick_scale / p;
        break;
    case kPan:
        // 0x0800A162
        Note(c);
        ch.pan = int8_t(p);
        v.pan = int8_t(p);
        v.flags |= kVoiceLevels;
        break;
    case kNoteDelay:
        // 0x0800A0F8: the note starts p ticks on.
        ch.counter = p;
        ch.routine = TickRoutine::kNoteDelay;
        ch.flags = uint8_t((ch.flags & ~kChannelTickMode) | 2);
        break;
    default:
        Note(c);
        break;
    }
}

} // namespace agblib::webfoot
