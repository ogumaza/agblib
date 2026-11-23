// SPDX-License-Identifier: MIT

// The levels and pitches of MP2K's tracks, the channels that their notes take, and the sequencer's reports.

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "agblib/agb/warnings.h"
#include "agblib/mp2k/sequencer.h"
#include "sequencer_shared.h"

namespace agblib::mp2k
{
using agblib::agb::WarnOnce;

namespace
{

// The rule of both channel searches among the notes that a new note can take: channel `c`, whose note has `priority`
// and track order `order`, replaces the one found so far when its priority is lower, or the same and its track order
// is as late or later.
void TakeIfBetter(int priority, int order, int c, int& lowest, int& latest, int& found)
{
    if (priority < lowest)
    {
        lowest = priority;
        latest = order;
        found = c;
    }
    else if (priority == lowest && order >= latest)
    {
        latest = order;
        found = c;
    }
}

} // namespace

Sequencer::SideLevels Sequencer::TrackLevels(const Track& track, bool metroid)
{
    // The Metroid games' sequencer gives a pan of -64 to 63 a side's level out of 128, from the centre's level out of
    // 64. Its tremolo multiplies the level by 65 plus the swing over 64, up to 255.
    if (metroid)
    {
        return MetroidLevels(track, track.pan);
    }

    // The volume and pan give each side a level. The pan moves the level in a straight line from one side to the
    // other. Each side's level is a byte. A level that the LFO raises past 255 wraps around.
    uint32_t level = (uint32_t(track.vol) * track.vol_x) >> 5;
    if (track.mod_t == 1)
    {
        level = (level * uint32_t(track.mod_m + 128)) >> 7;
    }

    int32_t pan = 2 * track.pan;
    if (track.mod_t == 2)
    {
        pan += track.mod_m;
    }

    pan = std::clamp(pan, -128, 127);

    return {uint8_t((uint32_t(127 - pan) * level) >> 8), uint8_t((uint32_t(pan + 128) * level) >> 8)};
}

Sequencer::SideLevels Sequencer::MetroidLevels(const Track& track, int pan)
{
    uint32_t level = (uint32_t(track.vol) * track.vol_x) >> 5;
    if (track.mod_t == 1)
    {
        level = uint32_t(std::min(255, (int32_t(level) * (track.mod_m + 65)) >> 6));
    }

    pan = std::clamp(pan, -64, 63);
    if (track.mod_t == 2)
    {
        pan = std::clamp(pan + track.mod_m, -64, 63);
    }

    return {uint8_t((uint32_t(64 - pan) * level) >> 7), uint8_t((uint32_t(pan + 64) * level) >> 7)};
}

void Sequencer::UpdateTrack(Track& track)
{
    if (track.flags & kFlagVolSet)
    {
        const SideLevels levels = TrackLevels(track, info_.metroid_sequencer);
        track.vol_mr = levels.right;
        track.vol_ml = levels.left;
    }

    // The pitch is in 256ths of a semitone: the key offset in the high bits, and the fine tune in the low 8.
    if (track.flags & kFlagPitSet)
    {
        const int32_t pitch = TrackPitch(track, info_.metroid_sequencer);
        track.key_m = int8_t(uint8_t(pitch >> 8));
        track.pit_m = uint8_t(pitch);
    }

    track.flags &= uint8_t(~(kFlagPitSet | kFlagVolSet));
}

void Sequencer::SetChannelVolume(Channel& c, const Track& track) const
{
    // The Metroid games' sequencer plays a note at its velocity plus 1 out of 128 of the track's levels. The mixer
    // works a drum kit's levels out again for each DirectSound note, at the pan that the note started with (0x08001006
    // in Metroid Fusion).
    if (info_.metroid_sequencer)
    {
        const SideLevels levels = (track.tone.type & kVoiceDrumKit) && !(c.type & kVoicePsg)
                                      ? MetroidLevels(track, c.start_pan)
                                      : SideLevels{track.vol_ml, track.vol_mr};
        c.right = uint8_t(std::min(((c.velocity + 1) * levels.right) >> 7, 255));
        c.left = uint8_t(std::min(((c.velocity + 1) * levels.left) >> 7, 255));
        return;
    }

    // Camelot's sequencer works out the left side's level from 128 less a drum kit voice's pan, where the driver takes
    // it from 127. Every note's left side therefore comes out a little louder.
    const int right = (c.velocity * (128 + c.rhythm_pan) * track.vol_mr) >> 14;
    const int left = (c.velocity * ((info_.camelot_sequencer ? 128 : 127) - c.rhythm_pan) * track.vol_ml) >> 14;
    c.right = uint8_t(std::min(right, 255));
    c.left = uint8_t(std::min(left, 255));
}

void Sequencer::SetChannelPitch(Channel& c, const Track& track) const
{
    const int key = std::max(0, c.key + track.key_m);
    const int psg = c.type & kVoicePsg;
    if (psg && psg != 4 && info_.metroid_psg_table)
    {
        c.frequency = MetroidPsgFrequency(key, track.pit_m);
    }
    else if (psg)
    {
        c.frequency = KeyToPsgFrequency(psg, key, track.pit_m);
    }
    else
    {
        c.frequency = SampleFrequency(info_, rom_.U32(c.wave + 4), key, track.pit_m);
    }
}

uint32_t Sequencer::MetroidPsgFrequency(int key, int fine) const
{
    // The Metroid games take a square or wave channel's setting for a key from their table, and add (fine + 1)/256 of
    // the step to the next key's for a fine tune above 0. The table's keys below 36 repeat the octave from 36 up
    // (0x08001E48 in Metroid Fusion).
    key = std::min(key, 127);
    const uint32_t table = info_.metroid_psg_table;
    const int low = rom_.U16(table + 2 * uint32_t(key));
    if (!fine)
    {
        return uint32_t(low);
    }

    const int high = rom_.U16(table + 2 * uint32_t(key + 1));
    return uint32_t(low + (((high - low) * (fine + 1)) >> 8)) & 0xFFFF;
}

void Sequencer::UpdateChannels()
{
    for (int t = 0; t < TrackCount(); t++)
    {
        Track& track = tracks_[size_t(t)];
        if (!(track.flags & kFlagExist) || !(track.flags & kFlagVolPitChange))
        {
            continue;
        }

        UpdateTrack(track);
        for (int c = track.chan; c >= 0;)
        {
            Channel& ch = channels_[size_t(c)];
            const int next = ch.next;
            if (!(ch.status & kStatusOn))
            {
                UnlinkChannel(c);
                c = next;
                continue;
            }

            if (track.flags & kFlagVolChange)
            {
                SetChannelVolume(ch, track);
                if (ch.type & kVoicePsg)
                {
                    ch.modify |= kModifyVolume;
                }
            }

            if (track.flags & kFlagPitChange)
            {
                SetChannelPitch(ch, track);
                if (ch.type & kVoicePsg)
                {
                    ch.modify |= kModifyPitch;
                }
            }

            c = next;
        }

        // The Metroid games' mixer leaves a drum kit's track at the pan of the last DirectSound note whose levels it
        // works out. It works through the channels in order.
        if (info_.metroid_sequencer && (track.tone.type & kVoiceDrumKit) && (track.flags & kFlagVolChange))
        {
            int last = -1;
            for (int c = track.chan; c >= 0; c = channels_[size_t(c)].next)
            {
                if (!(channels_[size_t(c)].type & kVoicePsg))
                {
                    last = std::max(last, c);
                }
            }

            if (last >= 0)
            {
                track.pan = channels_[size_t(last)].start_pan;
            }
        }

        track.flags &= uint8_t(~kFlagVolPitChange);
    }
}

int Sequencer::FindChannel(int t, int psg, int priority) const
{
    // A PSG voice has one channel. It takes it if it's free, or plays a released note, or a note of lower priority, or
    // one of the same priority from this track or a later one.
    if (psg)
    {
        const int c = kFirstPsgChannel + psg - 1;
        const Channel& ch = channels_[size_t(c)];
        if (!(ch.status & kStatusOn) || (ch.status & kStatusStop) || ch.priority < priority)
        {
            return c;
        }

        return ch.priority == priority && ch.track >= t ? c : -1;
    }

    // A sample takes the first free channel. Failing that, it takes a released note, or else a note of lower priority,
    // or of the same priority from this track or a later one: of those, the one with the lowest priority, from the
    // latest track, and the last in the list of channels.
    int found = -1;
    int lowest = priority;
    int latest = t + 1;
    bool released = false;
    for (int c = 0; c < direct_channels_; c++)
    {
        const Channel& ch = channels_[size_t(c)];
        if (!(ch.status & kStatusOn))
        {
            return c;
        }

        const int order = ch.track + 1;
        if ((ch.status & kStatusStop) && !released)
        {
            released = true;
            lowest = ch.priority;
            latest = order;
            found = c;
            continue;
        }

        if (!(ch.status & kStatusStop) && released)
        {
            continue;
        }

        TakeIfBetter(ch.priority, order, c, lowest, latest, found);
    }

    return found;
}

int Sequencer::FindCamelotChannel(int t, int priority) const
{
    // A sample takes the third free channel, in the list's order. With only one or two free, it takes the last of them,
    // unless one of this track's released notes has a channel: then it takes the last of those.
    const int count = direct_channels_;
    int free_count = 0;
    int last_free = -1;
    uint32_t released = 0;
    for (int c = 0; c < count; c++)
    {
        const Channel& ch = channels_[size_t(c)];
        if (!(ch.status & kStatusOn))
        {
            if (++free_count == 3)
            {
                return c;
            }

            last_free = c;
        }
        else if ((ch.status & kStatusStop) && !(info_.camelot_top_priority && ch.priority == 255))
        {
            // A later version's search leaves out a released note of priority 255 (0x0824ed3e in Mario Tennis: Power
            // Tour).
            released |= 1u << c;
        }
    }

    if (last_free >= 0)
    {
        for (int c = count - 1; c >= 0; c--)
        {
            if ((released & (1u << c)) && channels_[size_t(c)].track == t)
            {
                return c;
            }
        }

        return last_free;
    }

    // With none free, it takes a released note if there is one, or else a note of lower priority, or of the same
    // priority from this track or a later one: of those, the one with the lowest priority, from the latest track, and
    // the first in the list of channels.
    const uint32_t candidates = released ? released : (1u << count) - 1;
    int found = -1;
    int lowest = released ? 256 : priority;
    int latest = released ? 0 : t + 1;
    for (int c = count - 1; c >= 0; c--)
    {
        const Channel& ch = channels_[size_t(c)];
        if (!(candidates & (1u << c)))
        {
            continue;
        }

        const int order = ch.track + 1;
        TakeIfBetter(ch.priority, order, c, lowest, latest, found);
    }

    return found;
}

void Sequencer::LinkChannel(int index, int t)
{
    Channel& ch = channels_[size_t(index)];
    Track& track = tracks_[size_t(t)];
    ch.prev = -1;
    ch.next = track.chan;
    if (track.chan >= 0)
    {
        channels_[size_t(track.chan)].prev = index;
    }

    track.chan = index;
    ch.track = t;
    owners_[size_t(index)] = int8_t(t);
}

void Sequencer::UnlinkChannel(int index)
{
    Channel& ch = channels_[size_t(index)];
    if (ch.track < 0)
    {
        return;
    }

    if (ch.prev >= 0)
    {
        channels_[size_t(ch.prev)].next = ch.next;
    }
    else
    {
        tracks_[size_t(ch.track)].chan = ch.next;
    }

    if (ch.next >= 0)
    {
        channels_[size_t(ch.next)].prev = ch.prev;
    }

    ch.track = -1;
}

void Sequencer::ReportTrack(int t)
{
    Track& track = tracks_[size_t(t)];
    Action a;
    a.track = uint8_t(t);
    const uint8_t again = track.sets & track.restate;
    track.sets = 0;

    // The levels on each side. The driver computes them from the track's settings at the end of the frame, or when the
    // track starts a note.
    // A drum kit's notes in the Metroid games keep the pans that they started with. The levels follow the newest.
    const bool drums = info_.metroid_sequencer && (track.tone.type & kVoiceDrumKit) && track.chan >= 0;
    const SideLevels levels = drums ? MetroidLevels(track, channels_[size_t(track.chan)].start_pan)
                                    : TrackLevels(track, info_.metroid_sequencer);
    if (levels.left != track.last_left || levels.right != track.last_right || (again & (kSetVolume | kSetPan)))
    {
        track.restate &= uint8_t(~(kSetVolume | kSetPan));
        track.last_left = levels.left;
        track.last_right = levels.right;
        a.kind = Action::kLevels;
        a.left = levels.left;
        a.right = levels.right;
        AddAction(a);
    }

    const int pitch = TrackPitch(track, info_.metroid_sequencer);
    if (pitch != track.last_pitch || (again & kSetPitch))
    {
        track.restate &= uint8_t(~kSetPitch);
        track.last_pitch = pitch;
        a.kind = Action::kPitch;
        a.value = pitch;
        AddAction(a);
    }
}

int32_t Sequencer::TrackPitch(const Track& track, bool metroid)
{
    // The Metroid games' sequencer adds 1 to an upward bend. The highest bend then reaches the bend range. It moves the
    // tune in 16ths of a semitone, and the LFO's swing in steps of 48, while the LFO has a speed and a depth.
    if (metroid)
    {
        const int32_t bend = track.bend > 0 ? track.bend + 1 : track.bend;
        int32_t pitch = bend * track.bend_range * 4 + track.key_shift * 256 + track.tune * 16;
        if (track.mod_t == 0 && track.lfo_speed && track.mod)
        {
            pitch += 48 * track.mod_m;
        }

        return pitch;
    }

    int32_t pitch = (track.tune + track.bend * track.bend_range) * 4 + track.key_shift * 256;
    if (track.mod_t == 0)
    {
        pitch += 16 * track.mod_m;
    }

    return pitch;
}

void Sequencer::AddAction(Action action)
{
    action.tick = clock_;
    action.frame = frame_;
    actions_.push_back(action);
}

void Sequencer::ReleaseNote(int t, int channel)
{
    channels_[size_t(channel)].status |= kStatusStop;

    Action a;
    a.kind = Action::kRelease;
    a.track = uint8_t(t);
    a.channel = channel;
    AddAction(a);
}

void Sequencer::EndNote(int channel, bool sounded, bool taken)
{
    Action a;
    a.kind = Action::kNoteEnd;
    a.channel = channel;
    a.sounded = sounded;
    a.taken = taken;
    AddAction(a);
}

void Sequencer::CheckAudible(int channel)
{
    Channel& c = channels_[size_t(channel)];
    if (!c.audible && c.envelope > c.lag)
    {
        c.audible = true;
        Action a;
        a.kind = Action::kAudible;
        a.channel = channel;
        AddAction(a);
    }
}

void Sequencer::MarkSounded(int channel)
{
    const int owner = owners_[size_t(channel)];
    if (owner >= 0)
    {
        sounded_tracks_ = uint16_t(sounded_tracks_ | 1u << owner);
    }
}

void Sequencer::Warn(const std::string& message)
{
    WarnOnce(warnings_, message);
}

Hang::Hang(int track, uint32_t address)
    : std::runtime_error("track " + std::to_string(track) + " runs " + std::to_string(kCommandBudget) +
                         " commands in a tick without waiting, up to address " + xsflib::Hex(address, 0, true) +
                         ": the song hangs"),
      track_(track),
      address_(address)
{
}

int Hang::TrackIndex() const
{
    return track_;
}

uint32_t Hang::Address() const
{
    return address_;
}

} // namespace agblib::mp2k
