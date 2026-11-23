// SPDX-License-Identifier: MIT

// The voices of Rare's sequencer: the mixer, the render of each voice, the pitches, and the auditions.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/warnings.h"
#include "agblib/rare/driver.h"
#include "agblib/rare/sequencer.h"
#include "agblib/rare/song.h"

namespace agblib::rare
{

using agblib::agb::WarnOnce;

void Sequencer::Mix()
{
    // The mixer takes the voices whose notes are on, then those that are releasing, each in slot order, up to its
    // limit. The others are silent for the frame.
    for (Slot& s : slots_)
    {
        s.mix = s.state == kSlotOn || s.state == kSlotReleased ? MixResult::kSkipped : MixResult::kIdle;
    }

    // The routine before the mixer clears the mix buffer: all of it at 224, 304 and 352 samples a frame, and 88 of
    // its values at any other count.
    const auto samples = size_t(info_.samples_per_frame);
    if (enhance_)
    {
        std::fill(enhanced_.begin(), enhanced_.end(), 0.0f);
        std::fill(peaks_.begin(), peaks_.end(), 0);
    }

    if (render_)
    {
        const size_t cleared =
            samples == 224 || samples == 304 || samples == 352 ? samples : std::min<size_t>(88, samples);
        std::fill(mix_.begin(), mix_.begin() + std::ptrdiff_t(cleared), int16_t(0));
        std::fill(peaks_.begin(), peaks_.end(), 0);
    }

    // The mixer stops once it has taken as many voices as its limit allows. A limit of 0 never stops it.
    int left = info_.voice_limit;
    bool full = false;
    bool first = true;
    for (uint8_t state : {kSlotOn, kSlotReleased})
    {
        for (Slot& s : slots_)
        {
            if (full)
            {
                break;
            }
            if (s.state != state)
            {
                continue;
            }

            // A note's level before its velocity and envelope count is the music volume × (the channel's volume + 1) /
            // 128, × the channel's level / 128. Only the game's code turns a channel's level down, to 0 for the
            // channels in silenced_. In Donkey Kong Country 3's revision, a note at a level of 0 isn't mixed and
            // doesn't count towards the limit. Below a music volume of 128, that also leaves out the notes of a
            // channel at a volume of 0. A lower music volume leaves out those of more channel volumes.
            const bool silenced = Silenced(s.channel);
            if (info_.skips_silent_notes &&
                (silenced || (music_volume_ * (channels_[size_t(s.channel)].volume + 1)) >> 7 == 0))
            {
                s.mix = MixResult::kSilent;
                continue;
            }

            s.mix = MixResult::kMixed;
            MixVoice(s, first);
            first = false;
            full = --left == 0;
        }
    }

    // The mix, clipped to 8 bits, for FIFO A.
    if (render_)
    {
        for (size_t i = 0; i < samples; i++)
        {
            samples_[i] = int8_t(std::clamp<int>(mix_[i], -128, 127));
        }
    }
}

void Sequencer::MixVoice(Slot& s, bool first)
{
    s.step = PitchStep(s);

    // The mixer stops a sample without a loop at its end. It doesn't keep the position it reaches: Advance() moves it.
    Instrument inst;
    ReadInstrument(rom_, s.instrument, inst);
    if (render_)
    {
        RenderVoice(s, inst, first);
    }

    if (enhance_)
    {
        RenderEnhanced(s, inst);
    }

    const uint64_t moved = s.fraction + uint64_t(s.step) * uint32_t(info_.samples_per_frame);
    const uint32_t position = s.position + uint32_t(moved >> 23);
    const bool loops = LoopsForward(s.loop_mode);
    if (int32_t(position) >= int32_t(inst.end) && !loops)
    {
        s.state = kSlotFree;
    }
}

void Sequencer::RenderVoice(const Slot& s, const Instrument& inst, bool first)
{
    // The voice's level, out of 128: the music volume, scaled by the channel's volume, the velocity and the envelope,
    // each step truncated.
    uint32_t level = (music_volume_ * (channels_[size_t(s.channel)].volume + 1)) >> 7;
    level = Silenced(s.channel) ? 0 : level;
    level = (uint32_t(s.velocity) * level) >> 7;
    level = (uint32_t(s.level) * level) >> 19;

    // The bytes on either side of the position, interpolated by its 23-bit fraction. A sample past its end loops back
    // by the loop's length, a sample at a time, or else stops the voice for the rest of the frame.
    //
    // Banjo-Pilot's mixer writes the first voice's samples in pairs where the voice doesn't reach its sample's end in
    // the frame: the first sample, plus the second shifted up by 16 bits as one word. The first sample's sign borrows
    // 1 from the second where it's negative.
    const bool muted = muted_ >> s.track & 1;
    const uint32_t reached = s.position + uint32_t((uint64_t(mix_.size()) * s.step + s.fraction) >> 23);
    const bool pairs = info_.writes_pairs && first && int32_t(reached) < int32_t(inst.end);
    int32_t peak = 0;
    uint32_t position = s.position;
    uint32_t fraction = s.fraction;
    for (size_t i = 0; i < mix_.size(); i++)
    {
        const int32_t a = rom_.S8(position);
        const int32_t b = rom_.S8(position + 1);
        const int32_t point = a + (int32_t(fraction * uint32_t(b - a)) >> 23);
        const int32_t part = muted ? 0 : int32_t(uint32_t(point) * level) >> 7;
        const int32_t borrow = pairs && i % 2 == 1 && mix_[i - 1] < 0 ? 1 : 0;
        mix_[i] = int16_t(first ? part - borrow : mix_[i] + part);
        peak = std::max(peak, part < 0 ? -part : part);

        fraction += s.step;
        position += fraction >> 23;
        fraction &= 0x7FFFFF;
        if (int32_t(position) >= int32_t(inst.end))
        {
            if (!LoopsForward(s.loop_mode))
            {
                break;
            }

            position -= inst.loop_length;
        }
    }

    if (!enhance_)
    {
        int32_t& track = peaks_[s.track];
        track = std::max(track, peak);
    }
}

void Sequencer::RenderEnhanced(const Slot& s, const Instrument& inst)
{
    if ((muted_ >> s.track & 1) || Silenced(s.channel))
    {
        return;
    }

    // The level without the driver's truncations: the music volume, the channel's volume, the velocity and the
    // envelope, each out of its full scale.
    const double level = music_volume_ / 128.0 * (channels_[size_t(s.channel)].volume + 1) / 128.0 * s.velocity /
                         128.0 * double(s.level) / double(int64_t(1) << 19);

    // The driver's step moves the position by 1/2^23 bytes for each of its samples. The output's samples come at
    // agb::kEnhancedRate, over the same frame.
    const double step = double(s.step) / double(1 << 23) * double(info_.samples_per_frame) / double(enhanced_.size());
    const double end = double(int32_t(inst.end));
    const double loop = double(inst.loop_length);
    double position = double(s.position) + double(s.fraction) / double(1 << 23);
    double peak = 0;
    for (float& out : enhanced_)
    {
        const auto at = uint32_t(position);
        const double fraction = position - double(at);
        const double a = rom_.S8(at);
        const double b = rom_.S8(at + 1);
        const double part = (a + (b - a) * fraction) * level;
        out += float(part);
        peak = std::max(peak, std::abs(part));

        position += step;
        if (position >= end)
        {
            if (!LoopsForward(s.loop_mode) || loop <= 0)
            {
                break;
            }

            position -= loop * std::ceil((position - end + 1e-9) / loop);
        }
    }

    int32_t& track = peaks_[s.track];
    track = std::max(track, int32_t(std::lround(peak)));
}

void Sequencer::Advance(Slot& s)
{
    // Each frame moves every voice on by the frame's samples, whether it was mixed or not.
    const uint32_t step = s.step ? s.step : PitchStep(s);
    s.step = 0;
    const uint64_t moved =
        ((uint64_t(s.position) << 23) | s.fraction) + uint64_t(step) * uint32_t(info_.samples_per_frame);
    s.fraction = uint32_t(moved & 0x7FFFFF);
    s.position = uint32_t(moved >> 23);

    // Past the sample's end, a loop goes back by its length. In loop mode 1 the older revisions store the free state
    // through a register that doesn't contain the slot's address. The slot therefore stays in use until the mixer next
    // takes the voice.
    Instrument inst;
    ReadInstrument(rom_, s.instrument, inst);
    if (int32_t(s.position) < int32_t(inst.end) || (s.loop_mode == kLoopOnce && !info_.frees_loop_once))
    {
        return;
    }

    if (s.loop_mode == kLoopOnce || s.loop_mode == kLoopOnceB)
    {
        s.state = kSlotFree;
        return;
    }

    if (inst.loop_length == 0)
    {
        s.state = kSlotFree;
        Warn("an instrument loops with a loop length of 0");
        return;
    }

    while (int32_t(s.position) >= int32_t(inst.end))
    {
        s.position -= inst.loop_length;
    }
}

uint32_t Sequencer::PitchStep(const Slot& s) const
{
    Instrument voice;
    ReadInstrument(rom_, s.instrument, voice);

    // The pitch in semitones from the root key as 32.32 fixed point: the fine tune (cents), the bend and the vibrato.
    const int64_t total = kCent * voice.fine_tune + PitchOffset(s.channel);
    const uint32_t fraction = uint32_t(total);
    const int32_t semitones = int32_t(s.pitch_key) - int32_t(voice.root_key) + int32_t(total >> 32);

    // The pitch table gives 2^(n/12) in 9.23 fixed point, and the driver interpolates between its entries.
    uint32_t ratio = 0;
    if (info_.pitch_table)
    {
        const uint32_t at = info_.pitch_table + uint32_t(semitones) * 4;
        const uint32_t low = rom_.U32(at);
        const uint32_t high = rom_.U32(at + 4);
        ratio = low + uint32_t((uint64_t(fraction) * (high - low)) >> 32);
    }
    else
    {
        const double semis = semitones + fraction / 4294967296.0;
        ratio = uint32_t(std::lround(8388608.0 * std::pow(2.0, semis / 12.0)));
    }

    const uint32_t rate = uint32_t((uint64_t(info_.rate_scale) * voice.rate) >> 14);
    return uint32_t((uint64_t(ratio) * rate) >> 24);
}

int64_t Sequencer::PitchOffset(int channel) const
{
    if (channel < 0 || channel >= kChannels)
    {
        return 0;
    }

    // The bend and the vibrato's depth come from the channel's instrument. A voice may play another instrument.
    const ChannelState& c = channels_[size_t(channel)];
    Instrument inst;
    ReadInstrument(rom_, c.instrument, inst);
    const int32_t range = int32_t(uint32_t(inst.bend_range) * 0x80000u);
    const int64_t bend = int64_t(int32_t(c.bend - 0x2000)) * range;
    if (c.modulation == 0)
    {
        return bend;
    }

    // The vibrato's sine, interpolated between the table's 257 entries.
    const uint32_t index = c.vibrato_phase >> 16;
    const int32_t part = int32_t(c.vibrato_phase & 0xFFFF);
    int32_t sine = 0;
    if (info_.sine_table)
    {
        const int32_t a = rom_.S32(info_.sine_table + index * 4);
        const int32_t b = rom_.S32(info_.sine_table + index * 4 + 4);
        sine = a + int32_t((int64_t(b - a) * part) >> 16);
    }
    else
    {
        sine =
            int32_t(std::lround(4294967296.0 / 12800.0 * std::sin(c.vibrato_phase / 16777216.0 * 6.283185307179586)));
    }

    const int32_t scaled = int32_t(uint32_t(c.modulation + 1) * uint32_t(sine));
    return bend + int64_t(scaled) * inst.vibrato_depth;
}

void Sequencer::Audition(uint8_t program, uint8_t key, uint8_t velocity)
{
    if (!valid_)
    {
        throw std::logic_error("the tune's header can't be read");
    }

    audition_ = true;
    audition_key_ = uint8_t(key & 0x7F);
    Event e;
    e.channel = 0;
    e.a = uint8_t(program & 0x7F);
    Program(0, e);
    e.a = audition_key_;
    e.b = uint8_t(velocity & 0x7F);
    NoteOn(0, e);
}

void Sequencer::ReleaseAudition()
{
    if (!audition_)
    {
        return;
    }

    Event e;
    e.channel = 0;
    e.a = audition_key_;
    NoteOff(0, e);
}

void Sequencer::MixSamples(bool on)
{
    render_ = on;
    const auto samples = size_t(std::max(info_.samples_per_frame, 0));
    if (on && mix_.size() != samples)
    {
        mix_.assign(samples, 0);
        samples_.assign(samples, 0);
    }

    if (on)
    {
        peaks_.assign(tracks_.size(), 0);
    }
}

void Sequencer::MixEnhanced(bool on)
{
    enhance_ = on;
    enhanced_.assign(on ? size_t(agb::kEnhancedFrameSamples) : 0, 0.0f);
    peaks_.assign(tracks_.size(), 0);
}

bool Sequencer::Silenced(int channel) const
{
    // The driver gives every channel a level of 128 as it starts the tune, in the tune's first frame. The game's code
    // turns the levels down between frames.
    return frame_ > 0 && (silenced_ >> channel & 1);
}

void Sequencer::Warn(const std::string& message)
{
    WarnOnce(warnings_, message);
}

} // namespace agblib::rare
