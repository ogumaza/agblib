// SPDX-License-Identifier: MIT

// The Krawall player's rows: their entries, the effects between rows and the envelopes.

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

// An effect's entry in the player's table of effects.
struct EffectEntry
{
    bool between = false; // whether the effect runs on the ticks between rows as well as on the row's tick
    int second = 0;       // the effect whose part for the ticks between rows runs after it, or 0
};

constexpr EffectEntry EntryOf(int effect)
{
    switch (effect)
    {
    case kVolumeSlideS3m:
    case kVolumeSlideXm:
    case kPortaDownXm:
    case kPortaDownS3m:
    case kPortaUpXm:
    case kPortaUpS3m:
    case kPortaNote:
    case kVibrato:
    case kTremor:
    case kArpeggio:
    case kChannelVolumeSlide:
    case kPanSlide:
    case kRetrig:
    case kTremolo:
    case kFineVibrato:
    case kGlobalVolumeSlide:
    case kPanbrello:
    case kNoteCut:
    case kNoteDelay:
        return {.between = true};
    case kVolumeSlideVibrato:
    case kVolumeSlideVibratoXm:
        return {.between = true, .second = kVibrato};
    case kVolumeSlidePorta:
    case kVolumeSlidePortaXm:
        return {.between = true, .second = kPortaNote};
    default:
        return {};
    }
}

// Returns whether a volume column's effect runs on the ticks between rows.
constexpr bool ColumnBetween(int effect)
{
    switch (effect)
    {
    case kColumnVolumeSlideDown:
    case kColumnVolumeSlideUp:
    case kColumnVibrato:
    case kColumnPanSlideLeft:
    case kColumnPanSlideRight:
    case kColumnPortaNote:
        return true;
    default:
        return false;
    }
}

} // namespace

void Player::PlayRow()
{
    row_order_ = order_;
    row_number_ = row_;
    row_data_ = data_;
    row_loop_state_ =
        uint32_t(uint8_t(loop_row_)) | (uint32_t(uint8_t(loop_count_)) << 8) | (uint32_t(uint8_t(loop_active_)) << 16);

    while (status_ != 0)
    {
        const uint32_t at = data_;
        const uint8_t flags = rom_.U8(data_++);
        if (!flags)
        {
            break;
        }

        const RowEntry entry = ReadEntry(rom_, info_.build, flags, data_);
        channels_[size_t(entry.channel)].entry = at;
        PlayEntry(entry);
    }

    row_end_ = data_;

    EndRow();
    if (status_ != 0)
    {
        NextRow();
    }
}

void Player::PlayEntry(const RowEntry& entry)
{
    // The channel's note and instrument, volume and effect, whichever the entry gives.
    Channel& c = channels_[size_t(entry.channel)];
    c.in_row = 1;
    c.last_effect = c.effect;
    c.effect = entry.effect;

    bool sample_set = false;
    const bool instrument_volume = EntryInstrument(c, entry, sample_set);
    const bool set_volume = EntryVolume(c, entry) || instrument_volume;
    EntryNote(c, entry, sample_set);

    // The effect's part for the row, then the note or the volume.
    c.effect_op = entry.effect_op;
    if (entry.effect)
    {
        Effect(c, entry.effect, true);
    }

    if (c.play_note)
    {
        PlayNote(c);
    }
    else if (set_volume)
    {
        SetChannelVolume(c);
    }
}

bool Player::EntryInstrument(Channel& c, const RowEntry& entry, bool& sample_set)
{
    // An instrument sets the sample and its volume, and in a module with instruments, its pan too.
    if (!entry.instrument)
    {
        return false;
    }

    const uint32_t index = uint16_t(entry.instrument - 1);
    if (!instrument_based_)
    {
        c.sample = rom_.U32(info_.samples + 4 * index);
        c.sample_index = int(index);
        c.volume = c.played_volume = int8_t(rom_.U8(c.sample + kSampleVolume));
        return true;
    }

    if (!entry.note)
    {
        return false;
    }

    c.instrument = module_.instruments_at ? module_.instruments_at + kInstrumentSize * index
                                          : rom_.U32(info_.instruments + 4 * index);
    const uint32_t sample_index = rom_.U16(c.instrument + 2 * uint32_t(entry.note) - 2);
    c.sample = rom_.U32(info_.samples + 4 * sample_index);
    c.sample_index = int(sample_index);
    if (info_.build >= Build::k2003_09_01)
    {
        c.panning = c.played_panning = int8_t(rom_.U8(c.sample + kSamplePan));
    }

    c.volume = c.played_volume = int8_t(rom_.U8(c.sample + kSampleVolume));
    sample_set = true;

    return true;
}

bool Player::EntryVolume(Channel& c, const RowEntry& entry)
{
    // The volume column: a volume, or from 0x61 on, an effect. The builds before 1.4 take a volume up to 0x50, and
    // an effect from 0x51.
    if (!entry.volume)
    {
        return false;
    }

    if (entry.volume > (info_.build >= Build::kV1_4 ? 0x60 : 0x50))
    {
        c.column_effect = uint8_t(entry.volume - 0x60);
        VolumeColumnEffect(c, true);
        return false;
    }

    c.volume = int8_t(entry.volume - 0x10);
    if (c.volume > 64)
    {
        c.volume = 64;
    }

    c.played_volume = c.volume;

    // The builds before 1.2.2.14 keep the column's last effect. It goes on between the rows.
    if (info_.build >= Build::kV1_2_2_14)
    {
        c.column_effect = 0;
    }

    return true;
}

void Player::EntryNote(Channel& c, const RowEntry& entry, bool sample_set)
{
    // A note, unless the channel has no sample: a note off releases the note, a note with a slide to it sets the
    // slide's goal, and any other starts a note.
    c.play_note = 0;
    if (!entry.note || !c.sample)
    {
        return;
    }

    if (entry.note == kNoteOff)
    {
        NoteOff(c);
        return;
    }

    const uint8_t n = uint8_t(entry.note - 1 + rom_.U8(c.sample + kSampleRelativeNote));
    if (entry.effect == kPortaNote || entry.effect == kVolumeSlidePorta)
    {
        c.porta_goal = int16_t(PeriodOf(n, c.sample));
        return;
    }

    if (instrument_based_)
    {
        if (!sample_set)
        {
            const uint32_t sample_index = rom_.U16(c.instrument + 2 * uint32_t(n));
            c.sample = rom_.U32(info_.samples + 4 * sample_index);
            c.sample_index = int(sample_index);
        }

        // The builds before 2003/09/01 take the pan of the note's sample when the note starts.
        if (info_.build < Build::k2003_09_01)
        {
            c.panning = int8_t(rom_.U8(c.sample + kSamplePan));
        }

        // Up to 2003/09/01, the row starts the envelopes. Later builds start them with the note.
        if (!Since2004())
        {
            RestartEnvelopes(c);
        }
    }

    c.gliss_note = c.note = n;
    c.period = c.played_period = c.porta_goal = int16_t(PeriodOf(n, c.sample));
    c.tremor_count = 0;
    c.start_point = 0;
    c.play_note = 1;
}

void Player::Audition(uint16_t instrument, uint8_t note)
{
    audition_ = true;
    RowEntry entry;
    entry.note = note;
    entry.instrument = instrument;
    PlayEntry(entry);
    channels_[0].in_row = 0;
}

void Player::ReleaseAudition()
{
    RowEntry entry;
    entry.note = kNoteOff;
    PlayEntry(entry);
    channels_[0].in_row = 0;
}

void Player::EndRow()
{
    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (!c.in_row)
        {
            c.effect_op = 0;
            c.last_effect = c.effect;
            c.effect = 0;
            c.column_effect = 0;
        }
        else
        {
            c.in_row = 0;
        }

        if (c.restore_note && c.effect != c.last_effect)
        {
            SetChannelFreq(c);
            SetChannelVolume(c);
            SetChannelPan(c);
            c.restore_note = 0;
        }
    }
}

void Player::NoteOff(Channel& c)
{
    if (c.volume_env_state)
    {
        c.fading = 1;
        c.volume_env_state = c.volume_env_state == 2 ? uint8_t(3) : uint8_t(4);

        // The builds before 2003/09/01 release a pan envelope that's off too. That envelope then runs.
        if (c.pan_env_state || info_.build < Build::k2003_09_01)
        {
            c.pan_env_state = c.pan_env_state == 2 ? uint8_t(3) : uint8_t(4);
        }
    }
    else
    {
        StopChannel(c);
    }
}

void Player::RestartEnvelopes(Channel& c)
{
    c.envelopes_on = 0;
    c.instrument_vibrato_pos = 0;
    if (rom_.U8(c.instrument + kInstrumentVolumeEnvelope + kEnvelopeFlags) & kEnvelopeOn)
    {
        c.volume_env_state = 3;
        c.volume_env_tick = c.volume_env_point = 0xFF;
        c.volume_env_target = 0;
        if (info_.build < Build::kV1_2_2_14)
        {
            c.volume_env_tick = 0;
        }

        c.fade = 0x7FFF;
        c.fading = 0;
        c.envelopes_on = 1;
    }
    else
    {
        c.envelope_volume = 64;
        c.volume_env_state = 0;
    }

    if (rom_.U8(c.instrument + kInstrumentPanEnvelope + kEnvelopeFlags) & kEnvelopeOn)
    {
        c.pan_env_state = 3;
        c.pan_env_tick = c.pan_env_point = 0xFF;
        c.pan_env_target = 0;
        if (info_.build < Build::kV1_2_2_14)
        {
            c.pan_env_tick = 0;
        }

        c.envelopes_on = 1;
    }
    else
    {
        c.envelope_pan = 0;
        c.pan_env_state = 0;
    }

    // From 2004/09/17 on, an instrument's vibrato runs without envelopes too.
    if (Since2004() && rom_.U8(c.instrument + kInstrumentVibratoRate))
    {
        c.envelopes_on = 1;
    }
}

void Player::PlayTickBetweenRows()
{
    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (c.column_effect && ColumnBetween(c.column_effect >> 4))
        {
            VolumeColumnEffect(c, false);
        }

        if (!c.effect || c.effect >= kEffectCount)
        {
            continue;
        }

        const EffectEntry entry = EntryOf(c.effect);
        if (entry.between)
        {
            Effect(c, c.effect, false);
        }

        if (entry.second)
        {
            Effect(c, entry.second, false);
        }
    }
}

void Player::RunEnvelopes()
{
    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (!c.envelopes_on)
        {
            continue;
        }

        // A note that an envelope stops goes no further.
        const bool old = info_.build < Build::kV1_2_2_14;
        if (c.volume_env_state && !(old ? OldRunVolumeEnvelope(c) : RunVolumeEnvelope(c)))
        {
            continue;
        }
        if (c.pan_env_state > 2 && !(old ? OldRunPanEnvelope(c) : RunPanEnvelope(c)))
        {
            continue;
        }

        RunInstrumentVibrato(c);
    }
}

bool Player::RunVolumeEnvelope(Channel& c)
{
    // The volume envelope and the fade after the release.
    const uint32_t ins = c.instrument;
    bool set_volume = false;
    if (c.fading)
    {
        set_volume = true;
        c.fade = int16_t(c.fade - rom_.U16(ins + kInstrumentFade));
        if (c.fade <= 0)
        {
            StopChannel(c);
            return false;
        }
    }

    if (c.volume_env_state > 2)
    {
        set_volume = true;
        const uint32_t env = ins + kInstrumentVolumeEnvelope;
        c.volume_env_level = uint16_t(c.volume_env_level + rom_.U16(env + 4 * uint32_t(c.volume_env_point) + 2));
        c.volume_env_tick++;
        if (c.volume_env_tick == c.volume_env_target)
        {
            // The next point: a sustain point holds until the release, and the last one loops or stops.
            c.volume_env_point++;
            if ((rom_.U8(env + kEnvelopeFlags) & kEnvelopeSustains) &&
                c.volume_env_point == rom_.U8(env + kEnvelopeSustain) && c.volume_env_state != 4)
            {
                c.volume_env_state = 2;
            }

            if (c.volume_env_point == rom_.U8(env + kEnvelopeLast))
            {
                if (rom_.U8(env + kEnvelopeFlags) & kEnvelopeLoops)
                {
                    c.volume_env_point = rom_.U8(env + kEnvelopeLoopStart);
                    c.volume_env_tick = uint8_t(rom_.U16(env + 4 * uint32_t(c.volume_env_point)));
                }
                else
                {
                    c.volume_env_state = 1;
                    if ((rom_.U16(env + 4 * uint32_t(c.volume_env_point)) >> 9) == 0)
                    {
                        StopChannel(c);
                        return false;
                    }
                }
            }

            c.volume_env_level = uint16_t((rom_.U16(env + 4 * uint32_t(c.volume_env_point)) >> 9) << 8);
            c.volume_env_target = uint8_t(rom_.U8(env + 4 * uint32_t(c.volume_env_point) + 4) + 1);
        }
    }

    if (set_volume)
    {
        c.envelope_volume = int8_t((int32_t(c.volume_env_level) * c.fade) >> 23);
        if (!SetPlayedVolume(c))
        {
            StopChannel(c);
            return false;
        }
    }

    return true;
}

bool Player::RunPanEnvelope(Channel& c)
{
    const uint32_t env = c.instrument + kInstrumentPanEnvelope;
    c.pan_env_level = uint16_t(c.pan_env_level + rom_.U16(env + 4 * uint32_t(c.pan_env_point) + 2));
    c.pan_env_tick++;
    if (c.pan_env_tick == c.pan_env_target)
    {
        c.pan_env_point++;
        if ((rom_.U8(env + kEnvelopeFlags) & kEnvelopeSustains) && c.pan_env_point == rom_.U8(env + kEnvelopeSustain) &&
            c.pan_env_state != 4)
        {
            c.pan_env_state = 2;
        }

        if (c.pan_env_point == rom_.U8(env + kEnvelopeLast))
        {
            if (rom_.U8(env + kEnvelopeFlags) & kEnvelopeLoops)
            {
                c.pan_env_point = rom_.U8(env + kEnvelopeLoopStart);
                c.pan_env_tick = uint8_t(rom_.U8(env + 4 * uint32_t(c.pan_env_point)) + 1);
            }
            else
            {
                c.pan_env_state = 1;
            }
        }

        c.pan_env_level = uint16_t((rom_.U16(env + 4 * uint32_t(c.pan_env_point)) >> 9) << 8);
        c.pan_env_target = rom_.U8(env + 4 * uint32_t(c.pan_env_point) + 4);
    }

    c.envelope_pan = int8_t(((c.pan_env_level >> 8) - 32) << 1);
    if (!SetPlayedPan(c))
    {
        StopChannel(c);
        return false;
    }

    return true;
}

void Player::RunInstrumentVibrato(Channel& c)
{
    // The instrument's vibrato, on the pitch that the effects leave.
    const uint32_t ins = c.instrument;
    const uint8_t rate = rom_.U8(ins + kInstrumentVibratoRate);
    if (rate)
    {
        const int32_t wave = Wave(c.vibrato_wave, uint32_t(c.instrument_vibrato_pos) >> 2);
        const uint16_t period =
            uint16_t(uint16_t(c.played_period) + ((wave * rom_.U8(ins + kInstrumentVibratoDepth)) >> 8));
        SetFreq(c.handle, FrequencyOf(period));
        c.instrument_vibrato_pos = uint8_t(c.instrument_vibrato_pos + rate);
    }
}

} // namespace agblib::krawall
