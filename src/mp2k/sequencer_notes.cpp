// SPDX-License-Identifier: MIT

// The commands of MP2K's sequencer that start and end a track and its notes, and its extended and memory commands.

#include <algorithm>
#include <cstdint>

#include "ntdlib/commands/sequence.h"

#include "agblib/mp2k/sequencer.h"
#include "sequencer_shared.h"

namespace agblib::mp2k
{
using ntdlib::commands::Kind;

void Sequencer::StartTrack(Track& track)
{
    // The driver clears the first 64 bytes of the track's state. That leaves its position and pattern stack.
    Track fresh;
    fresh.position = track.position;
    fresh.stack = track.stack;
    fresh.flags = kFlagExist;
    fresh.bend_range = 2;
    fresh.vol_x = 64;

    // A game that gives the player a volume calls m4aMPlayImmInit first. That starts the tracks. m4aMPlayVolumeControl
    // then sets the started track's volX and the flag that recalculates its volumes.
    if (vol_x_ != fresh.vol_x)
    {
        fresh.vol_x = vol_x_;
        fresh.flags |= kFlagVolChange;
    }

    fresh.lfo_speed = 22;
    fresh.tone.type = 1;
    track = fresh;
}

void Sequencer::Note(int t, uint32_t command)
{
    Track& track = tracks_[size_t(t)];
    Action a;
    a.kind = Action::kNoteOn;
    a.track = uint8_t(t);
    a.command = command;
    a.key = track.key;
    a.velocity = track.velocity;
    a.gate = track.gate;
    a.program = track.voice;
    a.modified = track.modified || track.offset != 0;
    a.voice = track.voice >= 0 ? header_.voices + 12 * uint32_t(track.voice) : 0;

    // A key split or drum kit picks a voice for the key from its table. A drum kit's voice plays at the key and pan
    // that the voice gives.
    Voice voice = track.tone;
    uint8_t key = track.key;
    int8_t rhythm_pan = 0;
    if (track.tone.IsSplit())
    {
        const uint32_t index =
            track.tone.type & kVoiceKeySplit ? rom_.U8(track.tone.key_table + track.key) : uint32_t(track.key);
        a.voice = track.tone.wave + 12 * index;
        ReadVoice(rom_, a.voice, voice);
        if (voice.IsSplit())
        {
            a.kind = Action::kNoteDropped;
            AddAction(a);
            return;
        }

        if (track.tone.type & kVoiceDrumKit)
        {
            rhythm_pan = voice.RhythmPan();
            key = voice.key;

            // The Metroid games' sequencer gives the track a drum's pan: the centre for a byte of 0, the byte less 0x80
            // from 0x80 up, and the track's pan otherwise (0x0800481C in Metroid Fusion).
            if (info_.metroid_sequencer)
            {
                rhythm_pan = 0;
                if (voice.pan_sweep == 0 || (voice.pan_sweep & 0x80))
                {
                    track.pan = voice.pan_sweep ? int8_t(uint8_t(voice.pan_sweep - 0xC0)) : int8_t(0);
                    track.sets |= kSetPan;
                }
            }
        }
    }

    // The Metroid games' sequencer sets the LFO's swing to 0 and works the track's levels and pitch out again at each
    // note, before it looks for a channel. The track's other notes keep theirs, unless a command changed them.
    if (info_.metroid_sequencer)
    {
        if (track.lfo_delay)
        {
            track.lfo_delay_c = track.lfo_delay;
        }

        track.mod_m = 0;
        track.flags |= kFlagVolSet | kFlagPitSet;
        UpdateTrack(track);
    }

    // A later version of Camelot's sequencer keeps 255 for the notes of a player whose priority is 120, and others go
    // up to 254 (0x0824ecd0 in Mario Tennis: Power Tour).
    int priority = std::min(255, priority_ + track.priority);
    if (info_.camelot_top_priority)
    {
        priority = priority_ == kCamelotTopPlayer ? 255 : std::min(priority, 254);
    }

    const int psg = voice.PsgChannel();
    int c = -1;
    if (psg <= 4)
    {
        c = psg || !info_.camelot_sequencer ? FindChannel(t, psg, priority) : FindCamelotChannel(t, priority);

        // A sequencer that grows its channels gives the note a grown channel in place of a channel whose note sounds
        // before its release. A note in its release gives way. The console does the same.
        const uint8_t status = c >= 0 ? channels_[size_t(c)].status : 0;
        if (growing_ && (c < 0 || ((status & kStatusOn) && !(status & (kStatusStop | kStatusEcho)))))
        {
            if (const int grown = GrownChannel(psg); grown >= 0)
            {
                c = grown;
            }
        }
    }

    if (c < 0)
    {
        a.kind = Action::kNoteDropped;
        AddAction(a);
        return;
    }

    // The note takes the channel from any note that had it, and goes first in the track's list.
    Channel& ch = channels_[size_t(c)];
    if (ch.status & kStatusOn)
    {
        EndNote(c, !(ch.status & kStatusStart), true);
    }

    UnlinkChannel(c);
    LinkChannel(c, t);

    if (!info_.metroid_sequencer)
    {
        track.lfo_delay_c = track.lfo_delay;
        if (track.lfo_delay)
        {
            ClearModulation(track);
        }

        UpdateTrack(track);
    }

    ch.gate = track.gate;
    ch.midi_key = track.key;
    ch.velocity = track.velocity;
    ch.priority = uint8_t(priority);
    ch.key = key;
    ch.rhythm_pan = rhythm_pan;
    ch.start_pan = track.pan;
    ch.type = voice.type;
    ch.wave = voice.wave;
    ch.attack = voice.attack;
    ch.decay = voice.decay;
    ch.sustain = voice.sustain;
    ch.release = voice.release;
    ch.echo_volume = track.echo_volume;
    ch.echo_length = track.echo_length;
    SetChannelVolume(ch, track);

    SetChannelPitch(ch, track);
    if (!psg)
    {
        ch.count = int32_t(track.offset);
    }
    else
    {
        // Channel 1 sweeps by bits 4 to 6 of the voice's byte, unless they're 0 or bit 7 is set. 8 turns the sweep off.
        ch.length = voice.length;
        ch.sweep = (voice.pan_sweep & 0x80) || !(voice.pan_sweep & 0x70) ? 0x08 : voice.pan_sweep;
    }

    ch.status = kStatusStart;
    ch.synth = false;
    ch.audible = false;
    if (!info_.metroid_sequencer)
    {
        track.flags &= uint8_t(~kFlagVolPitChange);
    }

    a.channel = c;
    a.play_key = key;
    AddAction(a);
}

void Sequencer::EndTie(int t)
{
    // Release the newest of the track's notes with the key that hasn't been released yet.
    Track& track = tracks_[size_t(t)];
    for (int c = track.chan; c >= 0; c = channels_[size_t(c)].next)
    {
        Channel& ch = channels_[size_t(c)];
        if ((ch.status & (kStatusStart | kStatusEnvelope)) && !(ch.status & kStatusStop) && ch.midi_key == track.key)
        {
            ReleaseNote(t, c);
            break;
        }
    }
}

void Sequencer::EndTrack(int t)
{
    // The track's notes are released, and it lets go of its channels.
    Track& track = tracks_[size_t(t)];
    for (int c = track.chan; c >= 0;)
    {
        Channel& ch = channels_[size_t(c)];
        const int next = ch.next;
        if ((ch.status & kStatusOn) && !(ch.status & kStatusStop))
        {
            ReleaseNote(t, c);
        }

        UnlinkChannel(c);
        c = next;
    }

    track.flags = 0;
}

void Sequencer::ExtendedCommand(int t, const ntdlib::commands::Command& command)
{
    Track& track = tracks_[size_t(t)];
    const auto byte = uint8_t(command.value.value);

    // A key split keeps its key table where other voices keep their envelope.
    auto set_envelope = [&](uint8_t& field)
    {
        field = byte;
        track.tone.key_table = track.tone.attack | (track.tone.decay << 8) | (track.tone.sustain << 16) |
                               (uint32_t(track.tone.release) << 24);
        track.modified = true;
    };

    switch (command.kind)
    {
    case Kind::kSampleWave:
        track.tone.wave = uint32_t(command.value.value);
        track.modified = true;
        break;

    case Kind::kVoiceType:
        track.tone.type = byte;
        track.modified = true;
        break;

    case Kind::kAttack:
        set_envelope(track.tone.attack);
        break;

    case Kind::kDecay:
        set_envelope(track.tone.decay);
        break;

    case Kind::kSustain:
        set_envelope(track.tone.sustain);
        break;

    case Kind::kRelease:
        set_envelope(track.tone.release);
        break;

    case Kind::kEchoVolume:
        track.echo_volume = byte;
        break;

    case Kind::kEchoLength:
        track.echo_length = byte;
        break;

    case Kind::kPsgLength:
        track.tone.length = byte;
        track.modified = true;
        break;

    case Kind::kSweep:
        track.tone.pan_sweep = byte;
        track.modified = true;
        break;

    default:
        // kSampleOffset.
        track.offset = uint32_t(command.value.value);
        break;
    }
}

void Sequencer::MemAcc(Track& track, const ntdlib::commands::Command& command)
{
    // Operations 0-5 change the byte, with the value or with the byte the value names. Operations 6-17 jump if the byte
    // compares with them as each one says.
    uint8_t& byte = memory_[command.index];
    const auto value = uint8_t(command.value.value);
    const uint8_t operand = command.value.source == ntdlib::commands::Source::kVariable ? memory_[value] : value;
    bool jump = false;
    switch (command.kind)
    {
    case Kind::kSetVariable:
        byte = operand;
        return;
    case Kind::kAddVariable:
        byte = uint8_t(byte + operand);
        return;
    case Kind::kSubtractVariable:
        byte = uint8_t(byte - operand);
        return;
    case Kind::kJumpIfEqual:
        jump = byte == operand;
        break;
    case Kind::kJumpIfNotEqual:
        jump = byte != operand;
        break;
    case Kind::kJumpIfGreater:
        jump = byte > operand;
        break;
    case Kind::kJumpIfGreaterOrEqual:
        jump = byte >= operand;
        break;
    case Kind::kJumpIfLessOrEqual:
        jump = byte <= operand;
        break;
    default:
        jump = byte < operand;
        break;
    }

    if (jump)
    {
        track.position = command.target;
    }
}

void Sequencer::ClearModulation(Track& track)
{
    track.mod_m = 0;
    track.lfo_speed_c = 0;
    track.flags |= track.mod_t == 0 ? kFlagPitChange : kFlagVolChange;
    track.sets |= track.mod_t == 0 ? kSetPitch : track.mod_t == 1 ? kSetVolume : kSetPan;
}

} // namespace agblib::mp2k
