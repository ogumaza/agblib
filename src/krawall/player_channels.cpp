// SPDX-License-Identifier: MIT

// The Krawall player's channels: their notes, volumes, pans and pitches, and the S3M slides.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/warnings.h"
#include "agblib/krawall/build.h"
#include "agblib/krawall/driver.h"
#include "agblib/krawall/player.h"
#include "player_shared.h"

namespace agblib::krawall
{
namespace
{

// Returns `v` shifted right by the bottom byte of `amount` as an ARM register shift does.
uint32_t ShiftRight(uint32_t v, uint32_t amount)
{
    amount &= 0xFF;
    return amount >= 32 ? 0 : v >> amount;
}

} // namespace

void Player::PlayNote(Channel& c)
{
    c.restore_note = 0;
    if (!c.sample)
    {
        Warn("a note plays on a channel that has no sample yet, so it's left out");
        return;
    }

    // From 2004/09/17 on, a note in a module with instruments starts the envelopes.
    if (Since2004() && instrument_based_)
    {
        RestartEnvelopes(c);
    }

    const uint32_t freq = FrequencyOf(c.period);
    c.handle =
        Play(c.sample, c.sample_index, 0, c.handle, freq, uint32_t((c.volume * c.channel_volume) >> 6), c.panning);
    if (c.handle)
    {
        owners_[c.handle & 0x1F] = int8_t(&c - channels_.data());
    }

    if (info_.build == Build::kModified && c.handle)
    {
        MixChannel& m = mixer_[c.handle & 0xFF];
        m.hq_sample = rom_.U8(c.instrument + kInstrumentInterpolate);
        m.hq = hq_mode_ == 2 ? uint8_t(8) : uint8_t((m.hq_sample & hq_mode_) << 3);
    }

    if (c.start_point)
    {
        SetPos(c.handle, uint32_t(c.start_point) << 8);
    }
}

void Player::StopModule()
{
    if (status_ == 0)
    {
        return;
    }

    status_ = 0;
    SetTimer(false);
    for (int i = 0; i < module_.channels; i++)
    {
        StopChannel(channels_[size_t(i)]);
    }
}

void Player::SetGlobalVolume()
{
    SetMusicVolume((int32_t(module_.global_volume) * volume_ * music_volume_) >> 13);
}

bool Player::SetChannelVolume(Channel& c)
{
    c.played_volume = c.volume;
    return SetVol(c.handle, uint32_t((c.channel_volume * c.volume * c.envelope_volume) >> 12));
}

bool Player::SetPlayedVolume(Channel& c)
{
    if (!Since2004())
    {
        return SetChannelVolume(c);
    }

    return SetVol(c.handle, uint32_t((c.channel_volume * c.played_volume * c.envelope_volume) >> 12));
}

bool Player::SetChannelPan(Channel& c)
{
    c.played_panning = c.panning;
    const int pan = int8_t(uint8_t(c.panning + c.envelope_pan));
    return SetPan(c.handle, std::clamp(pan, -64, 64));
}

bool Player::SetPlayedPan(Channel& c)
{
    if (!Since2004())
    {
        return SetChannelPan(c);
    }

    return SetPan(c.handle, std::clamp(c.played_panning + c.envelope_pan, -64, 64));
}

bool Player::Since2004() const
{
    return info_.build >= Build::k2004_09_17;
}

void Player::SetChannelFreq(Channel& c)
{
    c.played_period = c.period;
    SetFreq(c.handle, FrequencyOf(c.period));
}

void Player::StopChannel(Channel& c)
{
    c.envelopes_on = 0;
    Stop(c.handle);
}

uint16_t Player::PeriodOf(uint32_t note, uint32_t sample) const
{
    const int fine_tune = rom_.S8(sample + kSampleFineTune);
    if (module_.linear)
    {
        return uint16_t(LinearPeriod(note, fine_tune));
    }

    return uint16_t(AmigaPeriod(rom_, info_, note, fine_tune));
}

uint32_t Player::FrequencyOf(int32_t period) const
{
    if (module_.linear)
    {
        return LinearFreq(uint32_t(period));
    }

    return uint32_t(Div(kAmigaClock, period).first);
}

uint32_t Player::LinearFreq(uint32_t period) const
{
    // The octave above the table's, and the step within it, of 768 to the octave.
    const uint32_t octave = ((period >> 6) * 0xAAAB) >> 19;
    const uint32_t step = period - octave * 768;
    return ShiftRight(rom_.U32(info_.linear + 4 * step), 23 - octave);
}

int16_t Player::Wave(uint32_t table, uint32_t index) const
{
    return int16_t(rom_.U16(table + 2 * index));
}

// ---------------------------------------------------------------------------------------------------------------------
// The effects

void Player::VolumeSlideS3m(Channel& c, bool on_row)
{
    // On the row, a fine slide (xF or Fx) slides once, and a normal one, in a module with fast slides, too.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.volume_slide = c.effect_op;
        }

        const int up = c.volume_slide >> 4, down = c.volume_slide & 0xF;
        c.volume_slide_once = 0;
        if (up && down == 0xF)
        {
            c.volume_slide_once = 1;
            VolumeUp(c, up);
        }
        else if (down && up == 0xF)
        {
            c.volume_slide_once = 1;
            VolumeDown(c, down);
        }

        if (c.effect == kVolumeSlideVibrato && c.restore_note && c.last_effect == kVibrato)
        {
            c.restore_note = 0;
        }

        if (!fast_slides_)
        {
            return;
        }
    }

    if (!c.volume_slide_once)
    {
        SlideVolume(c);
    }
}

void Player::PortaS3m(Channel& c, bool on_row, bool up)
{
    // On the row, Ex and Fx steps slide once, extra fine and fine; between rows, other steps slide by 4 times
    // themselves. Up lowers the period, and a period that reaches 0 stops the note.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.porta = c.effect_op;
        }

        if (c.porta < 0xE0)
        {
            return;
        }

        if ((c.porta & 0xF0) == 0xF0)
        {
            c.period = int16_t(c.period + (up ? -1 : 1) * ((c.porta & 0xF) << 2));
        }
        else if ((c.porta & 0xF0) == 0xE0)
        {
            c.period = int16_t(c.period + (up ? -1 : 1) * (c.porta & 0xF));
        }

        if (up && c.period <= 0)
        {
            StopChannel(c);
        }

        SetChannelFreq(c);
        return;
    }

    // A step of 0, or of 0xE0 or more, doesn't slide between rows.
    if (uint8_t(c.porta - 1) > 0xDE)
    {
        return;
    }

    if (up)
    {
        LowerPeriod(c, c.porta << 2);
    }
    else
    {
        RaisePeriod(c, c.porta << 2);
    }
}

} // namespace agblib::krawall
