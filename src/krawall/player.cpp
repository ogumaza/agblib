// SPDX-License-Identifier: MIT

#include "agblib/krawall/player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/warnings.h"
#include "agblib/krawall/build.h"
#include "agblib/krawall/driver.h"
#include "player_shared.h"

namespace agblib::krawall
{

using agblib::agb::Hex;
using agblib::agb::WarnOnce;

namespace
{

// The first 13 bytes of kramPlayExt()'s code. In IWRAM, the code follows the mixer's master volumes, quality and ramp.
// The bytes are the same in every build from 1.4 to 2005-04-21.
constexpr uint8_t kPlayExtStart[] = {0xF0, 0x47, 0x2D, 0xE9, 0x02, 0x50, 0xA0, 0xE1, 0xFF, 0x40, 0x05, 0xE2, 0x2C};

// The slowest tempo the sound timer takes: 16 times 24 ticks a minute.
constexpr int kMinTimerBpm = 16 * 24;

// The samples that the timer's step divides: 15 seconds of the mix.
constexpr int32_t kTimerSamples = 15 * 16384;

} // namespace

int PlayMode(const ModuleSong& song)
{
    if (song.mode >= 0)
    {
        return song.mode;
    }
    if (song.jingle)
    {
        return kModeJingle;
    }

    return song.song >= 0 ? kModeLoop | kModeSong : kModeLoop;
}

uint32_t AmigaPeriod(const Rom& rom, const DriverInfo& info, uint32_t note, int fine_tune)
{
    const uint32_t factor = rom.U16(info.fine_tunes + 2 * uint32_t(32 + (fine_tune >> 2)));
    return (rom.U16(info.periods + 2 * note) * factor) >> 15;
}

int LinearPeriod(uint32_t note, int fine_tune)
{
    return (fine_tune >> 1) + int(note << 6) + 0x600;
}

Player::Player(const Rom& rom, const DriverInfo& info, uint32_t module, int mode, int song) : rom_(rom), info_(info)
{
    owners_.fill(-1);
    if (!ReadModule(rom, info.build, module, module_))
    {
        Warn("the module at " + Hex(module) + " can't be read");
        return;
    }

    if (module_.channels > kPlayerChannels)
    {
        Warn("the module has " + std::to_string(module_.channels) + " channels, more than the player's " +
             std::to_string(kPlayerChannels));
        return;
    }

    // The mixer as the game's call to kramQualityMode() leaves it. Up to 2003/09/01, flag 16 turns the ramp at a stop
    // on. From 2004/09/17 on, the ramp is on, and flag 32 turns it off.
    if (Since2004())
    {
        hq_ramp_ = (info.quality & 0x20) ? uint8_t(0) : uint8_t(1);
    }
    else
    {
        hq_ramp_ = (info.quality & 0x10) ? uint8_t(1) : uint8_t(0);
    }

    hq_mode_ = (info.quality & 0xF) > 2 ? uint8_t(0) : uint8_t(info.quality & 0xF);
    if (info.build < Build::kV1_4)
    {
        // The builds before 1.4 have no ramping, and take the whole argument as the mode.
        hq_ramp_ = 0;
        hq_mode_ = info.quality > 2 ? uint8_t(0) : info.quality;
    }

    // The music volume, from 0 in a game that fades the music in.
    music_volume_ = info.music_fade_in ? 0 : int32_t(info.music_volume);

    // krapPlay(), with the player stopped: a jingle then plays as a module of its own. The first tick plays the first
    // row, at the module's volume and tempo.
    if (mode & kModeJingle)
    {
        mode &= ~kModeJingle;
    }

    mode_ = uint32_t(mode);
    speed_ = tick_ = module_.speed;
    volume_ = 64;
    volume_slide_ = 0;
    SetGlobalVolume();
    tempo_ = module_.tempo;
    SetTimerBpm(tempo_ * 24);

    // The song's first order, and the order that the module goes back to after its last.
    if (song > 63)
    {
        song = 0;
    }

    order_ = module_.song_starts[size_t(song)];
    song_start_ = order_;
    if (!(mode_ & kModeSong))
    {
        song_start_ = module_.restart;
    }

    row_ = 0;
    FindRow();

    goto_row_ = goto_order_ = -1;
    loop_row_ = loop_count_ = loop_active_ = 0;
    instrument_based_ = module_.instruments ? uint8_t(1) : uint8_t(0);
    fast_slides_ = module_.fast_slides ? uint8_t(1) : uint8_t(0);
    vibrato_offset_ = instrument_based_ ? uint8_t(32) : uint8_t(0);

    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        c.vibrato_pos = vibrato_offset_;
        c.vibrato_reset = 1;
        c.vibrato_wave = info.sine;
        c.tremolo_reset = 1;
        c.tremolo_wave = info.sine;
        c.panbrello_reset = 0;
        c.panbrello_wave = info.sine;
        c.panning = module_.channel_pan[size_t(i)];
        c.envelope_volume = 64;
        c.envelope_pan = 0;
        c.channel_volume = 64;
    }

    status_ = 2;
    SetTimer(true);
    valid_ = true;
}

Player::Player(const Rom& rom, const DriverInfo& info, const ModuleSong& song)
    : Player(rom, info, song.module, PlayMode(song), std::max(song.song, 0))
{
    if (valid_ && song.music_volume >= 0)
    {
        music_volume_ = song.music_volume;
        SetGlobalVolume();
    }
}

void Player::Warn(const std::string& text)
{
    WarnOnce(warnings_, text);
}

double Player::OutputScale() const
{
    // The mix adds each point times the side's volume, shifted down 3 bits, and its output shifts that down by the
    // master volume's shift and scales it by the master volume's level over 128. The builds before 1.4 take the whole
    // master volume as the level, and shift by 5.
    if (info_.build < Build::kV1_4)
    {
        return double(info_.master_volume) / double(1u << (3 + 5));
    }

    const uint32_t shift = (info_.master_volume >> 16) ? (info_.master_volume >> 16) : 5;
    return double(info_.master_volume & 0xFF) / double(1u << (3 + shift));
}

// ---------------------------------------------------------------------------------------------------------------------
// The mixer

uint32_t Player::Play(uint32_t sample, int sample_index, int sfx, uint32_t handle, uint32_t freq, uint32_t volume,
                      int32_t pan)
{
    // With ramping, the channel's note ramps down first, if it's still the channel's.
    MixChannel* m = &mixer_[handle & 0x1F];
    if (hq_ramp_ && m->handle == handle)
    {
        Stop(handle);
    }

    // A channel whose play is another's is left alone, and the play takes the first free channel instead.
    uint32_t channel = handle & 0xFF;
    if (!Plays(*m, handle))
    {
        channel = 0;
        while (channel < uint32_t(MixChannels()) && mixer_[channel].status != 0)
        {
            channel++;
        }
        if (channel >= uint32_t(MixChannels()))
        {
            return 0;
        }

        m = &mixer_[channel];
    }

    // The modified build's kramPlayExt() turns interpolation off. Its note on then sets it from the instrument.
    const bool modified = info_.build == Build::kModified;
    plays_++;
    m->sfx = uint8_t(sfx & 1);
    if (modified)
    {
        m->hq = 0;
    }
    else
    {
        m->hq = hq_mode_ == 2 ? uint8_t(8) : uint8_t((m->hq_sample & hq_mode_) << 3);
    }

    m->handle = channel | (plays_ << 8);
    SetPanning(*m, pan);
    m->volume = uint8_t(volume);
    m->inc = int32_t(freq << 2);
    SetSides(*m);
    m->hq_sample = modified ? uint8_t(0) : rom_.U8(sample + kSampleInterpolate);
    m->pos = m->start = sample + kSamplePoints;
    m->loop_length = rom_.U32(sample + kSampleLoopLength);
    m->end = SampleEnd(rom_, info_.build, sample);
    m->loop = rom_.U8(sample + kSampleLoop);
    m->status = 1;
    m->loop_from = m->end - m->loop_length;
    m->frac = 0;
    m->moves++;
    m->sample = sample;
    m->sample_index = sample_index;

    return m->handle;
}

bool Player::Plays(const MixChannel& m, uint32_t handle) const
{
    return (handle & 0xFF) < uint32_t(MixChannels()) && m.handle == handle;
}

bool Player::Stop(uint32_t handle)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    // Without ramping a stop is at once. With it, a channel that still plays loud enough ramps down from its point.
    if (!hq_ramp_ || m.status != 1 || m.volume <= 5)
    {
        m.status = 0;
        return true;
    }

    const uint8_t point = rom_.U8(m.pos);
    m.sfx = 16;
    m.loop_from = uint32_t(uint16_t(point << 4)) | (uint32_t(uint16_t(0x80 - point)) << 16);
    m.mix = 7;
    m.inc = 0;
    m.hq = 0;
    m.handle = 0;
    m.pos = m.start;

    return true;
}

bool Player::SetFreq(uint32_t handle, uint32_t freq)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    m.inc = m.inc >= 0 ? int32_t(freq << 2) : int32_t(0u - (freq << 2));

    return true;
}

bool Player::SetVol(uint32_t handle, uint32_t volume)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    m.volume = uint8_t(volume);
    SetSides(m);

    return true;
}

bool Player::SetPan(uint32_t handle, int32_t pan)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    SetPanning(m, pan);
    SetSides(m);

    return true;
}

bool Player::SetPos(uint32_t handle, uint32_t pos)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    if (m.start + pos < m.end)
    {
        m.pos = m.start + pos;
        m.frac = 0;
        m.moves++;
    }

    return true;
}

void Player::SetSides(MixChannel& m) const
{
    // A ramping channel keeps its count of samples to ramp (4 to 16) where the mixer keeps the choice of master volume.
    // The two master volumes are followed by the quality and the ramp, then by the code of kramPlayExt(). The mixer
    // reads the master volume of such a channel from those bytes.
    uint32_t master = 0;
    if (m.sfx < master_.size())
    {
        master = master_[m.sfx];
    }
    else if (m.sfx == 2)
    {
        master = hq_mode_;
    }
    else if (m.sfx == 3)
    {
        master = hq_ramp_;
    }
    else if (size_t(m.sfx - 4) < std::size(kPlayExtStart))
    {
        master = kPlayExtStart[m.sfx - 4];
    }

    const uint32_t level = master * m.volume;
    m.left = uint8_t((level * uint32_t(64 - m.pan)) >> 13);
    m.right = uint8_t((level * uint32_t(64 + m.pan)) >> 13);
}

void Player::SetPanning(MixChannel& m, int32_t pan) const
{
    // In mono, every channel mixes to the left buffer alone, at the level of a pan to the left. From 2005-04-21 on, and
    // in the unsigned and modified builds, it mixes at the level of the centre. A channel in the centre then has a
    // mixing routine of its own.
    const bool centre = info_.build >= Build::k2005_04_21;
    if (!info_.stereo)
    {
        m.mix = 1;
        m.pan = centre ? int8_t(0) : int8_t(-64);
        return;
    }

    if (pan == 0)
    {
        m.mix = centre ? uint8_t(4) : uint8_t(0);
    }
    else
    {
        m.mix = uint8_t(pan > 0 ? (pan >> 6) + 2 : (-pan) >> 6);
    }

    m.pan = int8_t(pan);
}

void Player::ResetChannels()
{
    // Up to 2003/09/01, every channel that plays takes the new master volume. From 2004/09/17 on, only the music's
    // channels do, and from 2004/11/14 on, they set their mixing routines again too.
    for (MixChannel& m : mixer_)
    {
        if (m.status == 0 || (Since2004() && m.sfx != 0))
        {
            continue;
        }

        if (info_.build >= Build::k2004_11_14)
        {
            SetPanning(m, m.pan);
        }

        SetSides(m);
    }
}

void Player::SetMusicVolume(int32_t volume)
{
    master_[0] = uint32_t(volume) > 255 ? uint8_t(255) : uint8_t(volume);
    ResetChannels();
}

void Player::SetTimerBpm(int bpm)
{
    bpm = std::max(bpm, kMinTimerBpm);
    timer_speed_ = timer_count_ = timer_backup_ = uint16_t((kTimerSamples / bpm) << 2);
}

void Player::SetTimer(bool on)
{
    timer_on_ = on;

    // The builds before 1.4 keep the timer's count as it is.
    if (info_.build < Build::kV1_4)
    {
        return;
    }

    if (on)
    {
        timer_count_ = timer_backup_;
    }
    else
    {
        timer_backup_ = timer_count_;
    }
}

void Player::FadeIn()
{
    if (info_.music_fade_in && status_ != 0)
    {
        music_volume_ = std::min(music_volume_ + int32_t(info_.music_fade_in), int32_t(info_.music_volume));
        SetGlobalVolume();
    }
}

void Player::SkipFrame()
{
    output_[0].clear();
    output_[1].clear();
    enhanced_[0].clear();
    enhanced_[1].clear();
    peaks_ = {};
    FadeIn();
}

void Player::Frame()
{
    if (mix_output_)
    {
        output_[0].clear();
        output_[1].clear();
    }

    enhanced_[0].clear();
    enhanced_[1].clear();
    peaks_ = {};
    FadeIn();

    if (info_.build < Build::kV1_4)
    {
        OldFrame();
        return;
    }

    // The worker mixes up to each tick that falls in the frame, runs it, and mixes the rest.
    uint32_t amount = uint32_t(info_.frame_samples);
    while (amount)
    {
        if (timer_count_ > amount || !timer_on_)
        {
            Mix(amount);
            timer_count_ = uint16_t(timer_count_ - amount);
            break;
        }

        const uint32_t count = timer_count_;
        Mix(count);
        amount -= count;
        Timer();
        if (on_tick_)
        {
            on_tick_(elapsed_);
        }

        timer_count_ = timer_speed_;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The player

void Player::Timer()
{
    // An audition runs the instrument's effects alone.
    ticks_++;
    if (audition_)
    {
        if (instrument_based_)
        {
            RunEnvelopes();
        }

        return;
    }

    tick_++;
    if (tick_ >= speed_)
    {
        tick_ = 0;
        PlayRow();
    }
    else
    {
        PlayTickBetweenRows();
    }

    if (instrument_based_)
    {
        RunEnvelopes();
    }
}

void Player::FindRow()
{
    // The order's pattern, from the module's table of patterns.
    pattern_ = rom_.U32(module_.address + ModulePatterns(info_.build) + 4 * uint32_t(module_.orders[order_]));
    data_ = PatternData(info_.build, pattern_);
    if (!rom_.Contains(pattern_, data_ - pattern_))
    {
        Warn("order " + std::to_string(order_) + " names a pattern outside the ROM, so the module stops there");
        StopModule();
        return;
    }

    if (row_ == 0)
    {
        return;
    }

    // The index gives every 4th row's data, and the rows between are skipped.
    data_ += rom_.U16(pattern_ + 2 * ((uint32_t(row_) >> 2) & 0xF));
    for (int left = row_ & 3; left > 0;)
    {
        const uint8_t flags = rom_.U8(data_++);
        if (!flags)
        {
            left--;
            continue;
        }

        ReadEntry(rom_, info_.build, flags, data_);
    }
}

void Player::NextRow()
{
    if (info_.build < Build::kV1_2_2_15)
    {
        OldNextRow();
        return;
    }

    if (info_.build < Build::k2003_09_01)
    {
        NextRowPlayer();
        return;
    }

    // A jump goes to its order's first row, through the order's end.
    const bool byte_rows = info_.build < Build::k2004_09_17;
    if (goto_order_ >= 0)
    {
        order_ = uint8_t(goto_order_ - 1);
        row_ = byte_rows ? 0xFE : 0x2000;
        goto_order_ = -1;
    }

    row_ = byte_rows ? uint8_t(row_ + 1) : uint16_t(row_ + 1);
    if (row_ < PatternRows(rom_, info_.build, pattern_) && goto_row_ < 0)
    {
        return;
    }

    // The next order, past the markers between songs (+++).
    const bool marker = NextOrder(module_, order_);

    // Past the last order, or at a marker in song mode, the module goes back to its restart or its song's start.
    if (order_ >= module_.order_count || (marker && (mode_ & kModeSong)))
    {
        order_ = uint8_t(song_start_);
        if (!(mode_ & kModeLoop))
        {
            StopModule();
            return;
        }
    }

    row_ = 0;
    FindRow();

    // A break goes to its row of the next order, or the first row if the pattern is shorter.
    if (goto_row_ >= 0)
    {
        row_ = uint32_t(goto_row_) >= PatternRows(rom_, info_.build, pattern_) ? uint16_t(0) : uint16_t(goto_row_);
        FindRow();
        goto_row_ = -1;
    }
}

} // namespace agblib::krawall
