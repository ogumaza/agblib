// SPDX-License-Identifier: MIT

#include "agblib/rare/sequencer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/rare/driver.h"
#include "agblib/rare/song.h"

namespace agblib::rare
{
namespace
{

// Maximum number of commands a track may run per frame. A loop without a delay in it would hang the driver.
constexpr int kMaxCommandsPerFrame = 100000;

// A quotient and remainder from the driver's 16-bit division.
struct Quotient
{
    uint32_t quotient;
    uint32_t remainder;
};

// Returns `dividend` / `divisor` and its remainder as the driver's 16-bit division works them out.
Quotient Divide(uint32_t dividend, uint32_t divisor)
{
    return {(dividend / divisor) & 0xFFFF, dividend % divisor};
}

// Frees a slot whose envelope has ended. The driver leaves a level of 0x10 in the slot.
void Stop(Slot& s)
{
    s.state = kSlotFree;
    s.level = 0x10;
}

// Takes a frame's step off a releasing note's level. Ends the note where the level would reach 0.
void FallRelease(Slot& s)
{
    if (s.level - s.level_step > 0)
    {
        s.level -= s.level_step;
    }
    else
    {
        Stop(s);
    }
}

} // namespace

uint32_t AttackSteps(uint32_t attack, uint8_t setting)
{
    return setting < 0x80 ? uint32_t(setting) : Divide((99 - std::min<uint32_t>(attack, 99)) << 5, 99).quotient;
}

uint32_t SustainLevel(uint32_t sustain, uint8_t setting)
{
    if (setting < 0x80)
    {
        return uint32_t(setting) << 8;
    }

    const Quotient whole = Divide(sustain << 7, 99);
    return (whole.quotient << 8) | Divide(whole.remainder << 8, 99).quotient;
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int tune, uint16_t silenced)
    : rom_(rom), info_(info), silenced_(silenced), music_volume_(uint32_t(MusicVolume(info, tune)))
{
    valid_ = ReadTuneHeader(rom, TuneAddress(rom, info, tune), header_);
    if (!valid_)
    {
        return;
    }

    for (uint32_t address : header_.tracks)
    {
        Track t;
        t.position = address;
        tracks_.push_back(t);
    }

    slots_.resize(size_t(kChannels * info.slots_per_channel));
    for (size_t i = 0; i < slots_.size(); i++)
    {
        slots_[i].channel = int8_t(i / size_t(info.slots_per_channel));
    }
}

const std::vector<Action>& Sequencer::Step()
{
    actions_.clear();
    if (!valid_)
    {
        return actions_;
    }

    // Once the tune has ended, the driver stops running the tracks and the envelopes, but goes on mixing.
    if (!ended_)
    {
        int at_end = 0;
        for (size_t t = 0; t < tracks_.size() && !audition_; t++)
        {
            if (RunTrack(int(t)))
            {
                at_end++;
            }
        }

        ended_ = !audition_ && at_end == int(tracks_.size());

        UpdateVibrato();
        for (size_t i = 0; i < slots_.size(); i++)
        {
            if (slots_[i].state == kSlotOn || slots_[i].state == kSlotReleased)
            {
                UpdateEnvelope(int(i));
            }
        }
    }

    for (Action& a : actions_)
    {
        a.frame = frame_;
    }

    Mix();

    for (Slot& s : slots_)
    {
        if (s.state == kSlotOn || s.state == kSlotReleased)
        {
            Advance(s);
        }
    }

    frame_++;

    return actions_;
}

bool Sequencer::RunTrack(int t)
{
    Track& track = tracks_[size_t(t)];
    track.counter -= frame_time_;
    if (track.counter >= 0)
    {
        return false;
    }
    if (track.done)
    {
        return true;
    }

    for (int n = 0; n < kMaxCommandsPerFrame; n++)
    {
        Event e;
        if (!DecodeEvent(rom_, track.position, info_.format, e))
        {
            char b[96];
            std::snprintf(b, sizeof b, "track %d has an unreadable command at 0x%08X; stopping the track", t,
                          unsigned(track.position));
            Warn(b);
            track.done = true;
            return true;
        }

        if (record_stretches_)
        {
            RecordRead(t, track.position, e.size);
        }

        track.position += e.size;
        switch (e.command)
        {
        case kCmdTempo:
            {
                tempo_ = e.value;
                frame_time_ = kFrameTime;
                Action a = TrackAction(Action::kTempo, t, e);
                a.value = e.value;
                actions_.push_back(a);
            }
            break;

        case kCmdDelay1:
        case kCmdDelay2:
        case kCmdDelay3:
            track.counter += DelayTime(e.command, e.value);
            track.tick += e.value;
            if (track.counter >= 0)
            {
                return false;
            }
            break;

        case kCmdNoteOnB:
        case kCmdNoteOn:
            NoteOn(t, e);
            break;

        case kCmdNoteOff:
            NoteOff(t, e);
            break;

        case kCmdControl:
            Control(t, e, track);
            break;

        case kCmdProgram:
            Program(t, e);
            break;

        case kCmdBend:
            channels_[e.channel].bend = e.value;
            actions_.push_back(TrackAction(Action::kBend, t, e));
            break;

        case kCmdEnd:
            // The driver reads the end again every frame, and counts the track as ended each time.
            track.position -= e.size;
            track.done = true;
            return true;

        default:
            break;
        }
    }

    Warn("track " + std::to_string(t) + " loops without a delay; it stops there");
    track.done = true;

    return true;
}

Action Sequencer::TrackAction(Action::Kind kind, int t, const Event& e) const
{
    Action a;
    a.kind = kind;
    a.track = uint8_t(t);
    a.channel = e.channel;
    a.tick = tracks_[size_t(t)].tick;
    a.address = tracks_[size_t(t)].position - e.size;

    return a;
}

void Sequencer::RecordRead(int t, uint32_t address, uint32_t size)
{
    Track& track = tracks_[size_t(t)];
    if (track.stretch >= 0 && stretches_[size_t(track.stretch)].last == address)
    {
        // A loop with no commands in it goes back to its end, and the track reads the end again. The read stays in the
        // stretch.
        return;
    }

    if (track.stretch >= 0 && address == track.next)
    {
        stretches_[size_t(track.stretch)].last = address;
    }
    else
    {
        TrackStretch stretch;
        stretch.track = uint8_t(t);
        stretch.tick = track.tick;
        stretch.first = address;
        stretch.last = address;
        stretch.loop = track.stretch >= 0;
        track.stretch = int(stretches_.size());
        stretches_.push_back(stretch);
    }

    track.next = address + size;
}

int64_t Sequencer::DelayTime(uint32_t command, uint32_t ticks) const
{
    // The delay in microseconds, a quotient and a remainder of ticks * tempo / ticks per quarter, with 8 bits of
    // fraction. A one-byte delay is multiplied in 32 bits.
    const uint32_t per_quarter = header_.ticks_per_quarter;
    const uint64_t product = command == kCmdDelay1 ? uint64_t(uint32_t(ticks * tempo_)) : uint64_t(tempo_) * ticks;
    const uint32_t quotient = uint32_t(product / per_quarter);
    const uint32_t remainder = uint32_t(product % per_quarter);
    const uint32_t fraction = Divide(remainder << 16, per_quarter).quotient;
    const uint32_t low = (quotient << 8) | (fraction >> 8);
    const uint32_t high = command == kCmdDelay1 ? 0 : quotient >> 24;
    return int64_t((uint64_t(high) << 32) | low);
}

void Sequencer::NoteOn(int t, const Event& e)
{
    Action a = TrackAction(Action::kNoteOn, t, e);
    a.a = e.a;
    a.b = e.b;

    // A mono channel always plays in its first slot. Otherwise the note takes the first free slot, or failing that the
    // first one whose note is releasing.
    const int base = e.channel * info_.slots_per_channel;
    const int end = base + info_.slots_per_channel;
    int index = -1;
    if (channels_[e.channel].mono)
    {
        index = base;
    }

    for (uint8_t want : {kSlotFree, kSlotReleased})
    {
        for (int i = base; i < end && index < 0; i++)
        {
            if (slots_[size_t(i)].state == want)
            {
                index = i;
            }
        }
    }

    if (index < 0)
    {
        a.kind = Action::kNoteDropped;
        actions_.push_back(a);
        return;
    }

    // A drum kit plays the instrument for the key at its root key, and a key split at the key. Any type of instrument
    // but a sample or a key split is read as a drum kit. The driver reads these words even where they aren't
    // instruments as it does for a channel without a program. Such a channel reads zeros here.
    const uint32_t top = channels_[e.channel].instrument;
    const Instrument chosen = InstrumentAt(rom_, top);
    uint32_t voice = top;
    if (!chosen.IsSample())
    {
        const uint8_t index_in_map = rom_.U8(chosen.key_map + e.a);
        voice = rom_.U32(chosen.key_table + 4u * index_in_map);
    }

    const Instrument played = InstrumentAt(rom_, voice);
    const bool drum_kit = !chosen.IsSample() && chosen.type != kInstKeySplit;

    Instrument inst;
    a.playable = ReadInstrument(rom_, voice, inst) && SampleValid(rom_, inst) && rom_.Contains(top);
    a.program = channels_[e.channel].program;
    a.instrument = top;
    if (!a.playable)
    {
        char b[160];
        std::snprintf(b, sizeof b,
                      top ? "track %d plays key %d on channel %d, whose instrument has no sample for it"
                          : "track %d plays key %d on channel %d before giving the channel a program; the game plays "
                            "it with the instrument the channel had in the song before",
                      t, e.a, e.channel);
        Warn(b);
    }

    Slot& s = slots_[size_t(index)];
    s.velocity = uint8_t(e.b + 1);
    s.loop_mode = uint8_t(played.loop_mode);
    s.key = e.a;
    s.pitch_key = drum_kit ? uint8_t(played.root_key) : e.a;
    s.position = played.start;
    s.fraction = 0;
    s.phase = kPhaseAttackStart;
    s.level = 0;
    s.instrument = voice;
    s.state = kSlotOn;
    s.track = uint8_t(t);
    a.slot = index;
    actions_.push_back(a);
}

void Sequencer::NoteOff(int t, const Event& e)
{
    // Only the channel's first slot playing the key is released: the driver goes back to the track once it has found
    // one.
    const int base = e.channel * info_.slots_per_channel;
    int released = -1;
    for (int i = base; i < base + info_.slots_per_channel; i++)
    {
        Slot& s = slots_[size_t(i)];
        if (s.state == kSlotOn && s.key == e.a)
        {
            s.state = kSlotReleased;
            released = i;
            break;
        }
    }

    Action a = TrackAction(Action::kNoteOff, t, e);
    a.slot = released;
    a.a = e.a;
    actions_.push_back(a);
}

void Sequencer::Control(int t, const Event& e, Track& track)
{
    ChannelState& c = channels_[e.channel];
    switch (e.a)
    {
    case kCtrlModulation:
        c.modulation = e.b;
        break;

    case kCtrlVolume:
        {
            // The model works out a voice's level only to find whether it's 0, and passes the volume on to the
            // conversion.
            c.volume = e.b;
            Action a = TrackAction(Action::kVolume, t, e);
            a.a = e.b;
            actions_.push_back(a);
        }
        break;

    case kCtrlLoopStart:
        track.loop = track.position;
        break;

    case kCtrlLoopEnd:
        if (track.loop == 0)
        {
            Warn("track " + std::to_string(t) + " ends a loop that it didn't start; the end is ignored");
            break;
        }

        track.position = track.loop;
        break;

    case kCtrlMonoOn:
    case kCtrlPolyOn:
        c.mono = e.a == kCtrlMonoOn;
        break;

    case kCtrlAttack:
    case kCtrlDecay:
    case kCtrlSustain:
    case kCtrlRelease:
        if (info_.envelope_controllers)
        {
            uint8_t* settings[] = {&c.envelope.attack, &c.envelope.decay, &c.envelope.sustain, &c.envelope.release};
            *settings[e.a - kCtrlAttack] = e.b;
        }
        break;

    default:
        break;
    }
}

void Sequencer::Program(int t, const Event& e)
{
    const uint8_t index = rom_.U8(header_.program_map + (e.a & 0x7Fu));
    if (index == 0xFF)
    {
        Warn("track " + std::to_string(t) + " chooses program " + std::to_string(e.a) +
             ", which the tune's program map doesn't have");
    }

    ChannelState& c = channels_[e.channel];
    c.instrument = rom_.U32(header_.instruments + 4u * index);
    c.program = e.a;
    c.envelope = EnvelopeSettings();

    Action a = TrackAction(Action::kProgram, t, e);
    a.a = e.a;
    a.instrument = c.instrument;
    actions_.push_back(a);
}

void Sequencer::UpdateVibrato()
{
    // A channel's vibrato phase runs while its modulation is on, at its instrument's rate, and goes back to 0 when the
    // modulation is turned off.
    for (ChannelState& c : channels_)
    {
        if (c.modulation == 0)
        {
            c.vibrato_phase = 0;
            continue;
        }

        Instrument inst;
        const uint32_t rate = ReadInstrument(rom_, c.instrument, inst) ? inst.vibrato_rate : 0;
        c.vibrato_phase = (c.vibrato_phase + rate) & 0xFFFFFF;
    }
}

void Sequencer::UpdateEnvelope(int index)
{
    Slot& s = slots_[size_t(index)];
    Instrument inst;
    ReadInstrument(rom_, s.instrument, inst);

    // The channel's settings from controllers 20-23 count where each phase starts.
    const EnvelopeSettings& settings = channels_[size_t(s.channel)].envelope;

    // Envelope phases advance within the same frame wherever the driver's code falls through. An update of a released
    // note in kPhaseAttack, kPhaseDecay or kPhaseSustain starts the release instead.
    switch (s.phase)
    {
    case kPhaseAttackStart:
        {
            // The attack takes `steps` + 1 frames, from 0 to the full level.
            AddEnvelopeAction(index, {.attack = settings.attack});
            const uint32_t steps = AttackSteps(inst.attack, settings.attack);
            if (steps == 0)
            {
                s.phase = kPhaseDecayStart;
                s.level = kFullLevel;
                return;
            }

            const Quotient q = Divide(kFullSustain, steps + 1);
            s.level = int32_t(q.remainder << 4);
            s.level_step = int32_t(q.quotient << 4);
            s.phase = kPhaseAttack;
        }
        [[fallthrough]];

    case kPhaseAttack:
        if (s.state == kSlotReleased)
        {
            StartRelease(index, inst);
        }
        else if (s.level + s.level_step >= kFullLevel)
        {
            s.phase = kPhaseDecayStart;
            s.level = kFullLevel;
        }
        else
        {
            s.level += s.level_step;
        }

        return;

    case kPhaseDecayStart:
        {
            // The decay falls from the full level to the sustain level in the fade table's number of frames. Where a
            // channel sets the sustain level but not the decay, the driver reads the decay from a register that
            // contains whatever the code before left there. The model takes the instrument's decay.
            AddEnvelopeAction(index, {.decay = settings.decay, .sustain = settings.sustain});
            const uint32_t sustain = SustainLevel(inst.sustain, settings.sustain);
            s.sustain = int32_t(sustain << 4);
            const int frames = FadeFrames(info_, inst.decay, settings.decay);
            if (frames == 0)
            {
                s.phase = kPhaseSustain;
                if (s.sustain < kAudibleLevel)
                {
                    Stop(s);
                }
                else
                {
                    s.level = s.sustain;
                }

                return;
            }

            const Quotient q = Divide(kFullSustain - sustain, uint32_t(frames));
            s.level_step = int32_t(q.quotient << 4);
            s.level -= int32_t(q.remainder << 4);
            s.phase = kPhaseDecay;
        }
        [[fallthrough]];

    case kPhaseDecay:
        {
            if (s.state == kSlotReleased)
            {
                StartRelease(index, inst);
                return;
            }

            int32_t level = s.level - s.level_step;
            if (level <= s.sustain)
            {
                s.phase = kPhaseSustain;
                level = s.sustain;
            }

            if (level < kAudibleLevel)
            {
                Stop(s);
            }
            else
            {
                s.level = level;
            }

            return;
        }

    case kPhaseSustain:
        if (s.state == kSlotReleased)
        {
            StartRelease(index, inst);
        }

        return;

    case kPhaseRelease:
        FallRelease(s);
        return;

    case kPhaseReleaseStart:
    default:
        StartRelease(index, inst);
        return;
    }
}

void Sequencer::StartRelease(int index, const Instrument& inst)
{
    // The release falls from the note's level to 0 in the fade table's number of frames.
    Slot& s = slots_[size_t(index)];
    const EnvelopeSettings& settings = channels_[size_t(s.channel)].envelope;
    AddEnvelopeAction(index, {.release = settings.release});
    const int frames = FadeFrames(info_, inst.release, settings.release);
    if (frames == 0)
    {
        Stop(s);
        return;
    }

    const Quotient q = Divide(uint32_t(s.level) >> 4, uint32_t(frames));
    if (q.quotient == 0)
    {
        Stop(s);
        return;
    }

    s.level_step = int32_t(q.quotient << 4);
    s.level -= int32_t(q.remainder << 4);
    s.phase = kPhaseRelease;
    FallRelease(s);
}

void Sequencer::AddEnvelopeAction(int index, const EnvelopeSettings& used)
{
    if (used.None())
    {
        return;
    }

    Action a;
    a.kind = Action::kEnvelope;
    a.channel = uint8_t(slots_[size_t(index)].channel);
    a.slot = index;
    a.envelope = used;
    actions_.push_back(a);
}

} // namespace agblib::rare
