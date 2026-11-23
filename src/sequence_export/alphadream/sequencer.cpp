// SPDX-License-Identifier: MIT

// The model of AlphaDream's sound driver. Each routine's address in A88E is given where the model follows it.

#include "agblib/sequence_export/alphadream/sequencer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/alphadream/driver.h"
#include "sequencer_internal.h"

namespace agblib::sequence_export::alphadream
{

std::array<uint8_t, 32> TrackState::Bytes() const
{
    std::array<uint8_t, 32> b{};
    b[0] = uint8_t(flags);
    b[1] = uint8_t(flags >> 8);
    b[2] = tempo;
    b[3] = count;
    for (int i = 0; i < 4; i++)
    {
        b[size_t(4 + i)] = uint8_t(data >> (8 * i));
    }

    b[8] = uint8_t(at);
    b[9] = uint8_t(at >> 8);
    b[0xA] = wait;
    b[0xB] = key;
    b[0xC] = instrument;
    b[0xD] = volume;
    b[0xE] = pan;
    b[0xF] = bend_range;
    b[0x10] = uint8_t(bend);
    b[0x11] = uint8_t(fine);
    b[0x12] = channel;
    b[0x13] = player;
    b[0x14] = level_lfo.depth;
    b[0x15] = level_lfo.period;
    b[0x16] = level_lfo.delay;
    b[0x17] = level_lfo.count;
    b[0x18] = pitch_lfo.depth;
    b[0x19] = pitch_lfo.period;
    b[0x1A] = pitch_lfo.delay;
    b[0x1B] = pitch_lfo.count;
    for (size_t i = 0; i < 4; i++)
    {
        b[0x1C + i] = loops[i];
    }

    return b;
}

std::array<uint8_t, 12> SampleChannel::TraceBytes() const
{
    return {status,      level,       uint8_t(sample), uint8_t(sample >> 8),
            right,       left,        uint8_t(pitch),  uint8_t(pitch >> 8),
            envelope[0], envelope[1], envelope[2],     envelope[3]};
}

std::array<uint8_t, 12> PsgChannel::TraceBytes() const
{
    return {envelope[0],
            envelope[1],
            envelope[2],
            envelope[3],
            volume,
            count,
            duty,
            state,
            uint8_t(frequency),
            uint8_t(frequency >> 8),
            written,
            0};
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, const Song& song) : rom_(rom), info_(info)
{
    open_.fill(-1);
    held_.fill(-1);

    // A84J's init leaves the player's volume at 0. Only the game's fades set it.
    if (info.version == Version::kHamtaro4)
    {
        player_volume_ = 0;
    }

    // The driver's init gives each track its channel and player (0x0819AFA8). The start gives each of the song's tracks
    // its first settings (0x0819B040). The flags start with kMuted, and the first frame therefore restores the channel.
    for (int t = 0; t < kTracks; t++)
    {
        TrackState& s = tracks_[size_t(t)];
        s.channel = uint8_t(t);
        s.player = kMusicPlayer;
        if (!(song.mask & (1 << t)))
        {
            continue;
        }

        // B85A's driver starts a track without a note. The first frame therefore starts none.
        s.flags =
            info.version == Version::kHamSports ? flag::kMuted | flag::kUsed : flag::kNote | flag::kMuted | flag::kUsed;
        s.data = song.tracks[size_t(t)];
        s.at = 0;
        s.tempo = 0x78;
        s.count = 0;
        s.instrument = 0;
        s.pan = 0x7F;
        s.wait = 1;
        s.volume = 0xC8;
        s.bend = 0;
        s.bend_range = 2;
        s.fine = 0;
    }
}

uint8_t Sequencer::Next(TrackState& t)
{
    const uint8_t b = rom_.U8(t.data + t.at);
    t.at = uint16_t(t.at + 1);

    return b;
}

bool Sequencer::Playing() const
{
    for (const TrackState& t : tracks_)
    {
        if (t.flags & flag::kUsed)
        {
            return true;
        }
    }

    for (const SampleChannel& c : samples_)
    {
        if (c.status)
        {
            return true;
        }
    }

    return PsgSounds(0) || PsgSounds(1) || PsgSounds(kNoiseChannel);
}

bool Sequencer::Looped(int passes) const
{
    // A track that plays on has to have gone back. A track that has ended doesn't count.
    bool any = false;
    for (int t = 0; t < kTracks; t++)
    {
        if (tracks_[size_t(t)].flags & flag::kUsed)
        {
            if (int(goto_frames_[size_t(t)].size()) < passes)
            {
                return false;
            }

            any = true;
        }
    }

    return any;
}

uint32_t Sequencer::LoopFrame(int pass) const
{
    uint32_t frame = 0;
    for (const std::vector<uint32_t>& frames : goto_frames_)
    {
        if (int(frames.size()) > pass)
        {
            frame = std::max(frame, frames[size_t(pass)]);
        }
    }

    return frame;
}

// The pitch of a sample channel (0x0819A79C) or a PSG channel (0x0819AA38, 0x0819AA9C). With `square`, the noise
// takes the squares' setting too as the restore of a PSG channel gives it (0x0819ABF8).
void Sequencer::UpdatePitch(int index, bool square)
{
    TrackState& t = tracks_[size_t(index)];
    const int scale = index < kSampleTracks ? 2 : 1;
    uint16_t p = uint16_t((t.key << 8) + t.bend * (t.bend_range * scale));
    p = uint16_t(p + t.fine);
    if (t.flags & flag::kPitchLfo)
    {
        if (t.flags & flag::kPitchLfoOn)
        {
            p = uint16_t(p + ((t.flags & flag::kPitchLfoDown) ? -int(t.pitch_lfo.depth) : int(t.pitch_lfo.depth)));
        }
    }

    if (index < kSampleTracks)
    {
        samples_[t.channel & 7].pitch = p;
        return;
    }

    PsgChannel& c = psg_[t.channel & 3];
    if ((t.channel & 3) == 3 && !square)
    {
        // The noise's setting for the key alone (0x0819A9B4).
        c.frequency = t.key <= 0x14 ? 0xD7 : t.key <= 0x50 ? rom_.U8(info_.noise_table + t.key - 0x15) : 0;
        return;
    }

    const int key = (p >> 8) - 36;
    if (key < 0)
    {
        c.frequency = 0;
        return;
    }

    const int lo = rom_.U16(info_.psg_pitch_table + 2 * uint32_t(key));
    const int hi = rom_.U16(info_.psg_pitch_table + 2 * uint32_t(key + 1));
    c.frequency = uint16_t(0x800 - (lo + (((hi - lo) * (p & 0xFF)) >> 8)));
}

// A sample channel's volumes (0x0819A834), or a PSG channel's volume and sides (0x0819ABB8, 0x0819AB78).
void Sequencer::UpdateLevels(int index)
{
    TrackState& t = tracks_[size_t(index)];
    // A84J's driver applies the player's volume only while it fades the player.
    const int player = info_.version == Version::kHamtaro4 ? 0x100 : (player_volume_ >> 8) + 1;
    if (index >= kSampleTracks)
    {
        PsgChannel& c = psg_[t.channel & 3];
        c.volume = uint8_t((t.volume * player) >> 12);
        c.right = t.pan <= 0x80;
        c.left = t.pan >= 0x7F;
        return;
    }

    int near = 0x7F;
    int far = t.pan;
    if (int8_t(t.pan) < 0)
    {
        near = (0xFF - t.pan) & 0xFF;
        far = 0x7F;
    }

    int v = t.volume;
    if ((t.flags & flag::kLevelLfo) && (t.flags & flag::kLevelLfoOn))
    {
        v += (t.flags & flag::kLevelLfoDown) ? -int(t.level_lfo.depth) : int(t.level_lfo.depth);
    }

    v = std::clamp(v, 0, 0xFF);
    v = (v * player) >> 8;
    SampleChannel& c = samples_[t.channel & 7];
    c.right = uint8_t(((v + 1) * near) >> 8);
    c.left = uint8_t(((v + 1) * far) >> 8);
}

// Starts a note on the track's channel (0x0819A7EC, 0x0819AAE4, 0x0819AB30). `command` is the address of the note's
// bytes, or 0 for the channel's restore.
void Sequencer::NoteOn(int index, uint32_t command)
{
    TrackState& t = tracks_[size_t(index)];
    Region region;
    if (index < kSampleTracks)
    {
        SampleChannel& c = samples_[t.channel & 7];
        c.status = 0;
        EndNote(index, frame_);
        LetGo(index, frame_);
        if (!FindRegion(rom_, info_, t.instrument, t.key, region))
        {
            return;
        }

        UpdatePitch(index);
        c.sample = region.sample;
        c.envelope = region.envelope;
        c.position = 0;
        c.status = 0x80;
    }
    else
    {
        if (!FindRegion(rom_, info_, t.instrument, t.key, region))
        {
            return;
        }

        EndNote(index, frame_);
        LetGo(index, frame_);

        // The wave's note-on (0x0819AB30) loads the region's pattern and keeps the record's other bytes.
        PsgChannel& c = psg_[t.channel & 3];
        if ((t.channel & 3) != kWaveChannel)
        {
            c.envelope = region.envelope;
            c.duty = uint8_t(region.sample);
        }

        c.state = 1;
    }

    Note n;
    n.track = index;
    n.on = frame_;
    n.key = t.key;
    n.instrument = t.instrument;
    n.region = region;
    n.command = command;
    open_[size_t(index)] = int(notes_.size());
    held_[size_t(index)] = int(notes_.size());
    notes_.push_back(n);
    last_[size_t(index)] = Change{index, frame_, -1000, -1, -1};
}

// Releases the track's note (0x0819A8C0, 0x0819AAC0). A square's or the noise's note sounds on through the release,
// until the hardware's envelope falls silent. The wave falls silent on the same frame.
void Sequencer::NoteOff(int index)
{
    TrackState& t = tracks_[size_t(index)];
    LetGo(index, frame_);

    if (index < kSampleTracks)
    {
        // A note that starts on the same frame plays on through the release until its channel stops.
        samples_[t.channel & 7].status |= 0x40;
        const int open = open_[size_t(index)];
        if (open >= 0 && notes_[size_t(open)].on == frame_)
        {
            return;
        }
    }
    else
    {
        psg_[t.channel & 3].state = 5;
        if ((t.channel & 3) != kWaveChannel)
        {
            return;
        }
    }

    EndNote(index, frame_);
}

void Sequencer::EndNote(int index, uint32_t frame)
{
    const int open = open_[size_t(index)];
    if (open >= 0)
    {
        notes_[size_t(open)].off = frame;
        notes_[size_t(open)].ended = true;
        open_[size_t(index)] = -1;
    }
}

// Records the frame on which the track lets its note go: the note's release, the track's next note or the end of a
// sample. A square's note can sound on after it, or fall silent before it.
void Sequencer::LetGo(int index, uint32_t frame)
{
    const int held = held_[size_t(index)];
    if (held >= 0)
    {
        notes_[size_t(held)].released = frame;
        held_[size_t(index)] = -1;
    }
}

void Sequencer::KeepRead(int index, bool ticks, uint32_t address)
{
    if (keep_reads_)
    {
        reads_.push_back({uint8_t(index), ticks, frame_, address});
    }
}

// Runs a track for a frame (0x0819B450).
void Sequencer::StepTrack(int index)
{
    TrackState& t = tracks_[size_t(index)];
    if (t.flags & flag::kNote)
    {
        // The square LFOs (0x0819A72C, 0x0819A6A8). Each turns on after its delay and swings each period after that.
        if (t.flags & flag::kPitchLfo)
        {
            if (t.pitch_lfo.count)
            {
                t.pitch_lfo.count--;
            }
            else
            {
                t.pitch_lfo.count = t.pitch_lfo.period;
                if (t.flags & flag::kPitchLfoOn)
                {
                    t.flags ^= flag::kPitchLfoDown;
                    UpdatePitch(index);
                }
                else
                {
                    t.flags |= flag::kPitchLfoOn;
                }
            }
        }

        if (t.flags & flag::kLevelLfo)
        {
            if (t.level_lfo.count)
            {
                t.level_lfo.count--;
            }
            else
            {
                t.level_lfo.count = t.level_lfo.period;
                if (t.flags & flag::kLevelLfoOn)
                {
                    t.flags ^= flag::kLevelLfoDown;
                    UpdateLevels(index);
                }
                else
                {
                    t.flags |= flag::kLevelLfoOn;
                }
            }
        }
    }

    uint32_t count = uint32_t(t.count) + t.tempo;
    int ticks = 0;
    while (count > uint32_t(kDefaultDivisor))
    {
        ticks++;
        count -= uint32_t(kDefaultDivisor);
    }

    t.count = uint8_t(count);
    if (ticks == 0)
    {
        return;
    }

    enum class At
    {
        kWait,
        kNoteEnd,
        kEvent
    };
    At next = At::kWait;
    for (int guard = 0; guard < 100000; guard++)
    {
        if (next == At::kWait)
        {
            ticks -= t.wait;
            if (ticks < 0)
            {
                t.wait = uint8_t(-ticks);
                return;
            }

            next = At::kNoteEnd;
        }

        if (next == At::kNoteEnd)
        {
            // Once a note has set the rests' state, a rest follows each note that isn't tied, where the note ends
            // (0x0819B4C0). The end of any other wait reads the next command.
            if ((t.flags & (flag::kNote | flag::kTie)) == flag::kNote)
            {
                t.flags &= uint16_t(~(flag::kNote | flag::kTie));
                NoteOff(index);
                if (t.flags & flag::kRests)
                {
                    KeepRead(index, true, t.data + t.at);
                    t.wait = Next(t);
                    next = At::kWait;
                    continue;
                }
            }

            next = At::kEvent;
        }

        const uint32_t event_at = t.at;
        KeepRead(index, false, t.data + event_at);
        const uint8_t b = Next(t);
        switch (b)
        {
        case 0xE0:
            t.pitch_lfo.depth = Next(t);
            t.pitch_lfo.period = Next(t);
            t.pitch_lfo.delay = Next(t);
            continue;

        case 0xE1:
            t.flags |= flag::kPitchLfo;
            continue;

        case 0xE2:
            t.flags &= uint16_t(~flag::kPitchLfo);
            continue;

        case 0xE3:
            t.level_lfo.depth = Next(t);
            t.level_lfo.period = Next(t);
            t.level_lfo.delay = Next(t);
            continue;

        case 0xE4:
            t.flags |= flag::kLevelLfo;
            continue;

        case 0xE5:
            t.flags &= uint16_t(~flag::kLevelLfo);
            continue;

        case 0xE8:
        case 0xE9:
        case 0xEA:
        case 0xEB:
        case 0xEC:
        case 0xED:
        case 0xEE:
        case 0xEF:
            {
                // A count, then a 16-bit jump from after the command, with one of four counters. Each command adds 1 to
                // its counter. While the count is at least the counter, E8-EB clear the counter, and EC-EF jump and
                // clear it. Once the counter passes the count, E8-EB jump and EC-EF go on. No song of A88E uses them.
                const int times = Next(t);
                const int lo = Next(t);
                const uint16_t jump = uint16_t(lo | (Next(t) << 8));

                uint8_t& counter = t.loops[b & 3];
                const int now = counter + 1;
                counter = uint8_t(now);
                const bool within = times >= now;
                if (within)
                {
                    counter = 0;
                }

                if (within == (b >= 0xEC))
                {
                    t.at = uint16_t(t.at + jump);
                }

                continue;
            }

        case 0xF0:
            t.instrument = Next(t);
            continue;

        case 0xF1:
            // A note's length in B85A and A84J.
            if (info_.version != Version::kSuperstarSaga)
            {
                break;
            }

            t.volume = Next(t);
            UpdateLevels(index);
            continue;

        case 0xF2:
            t.pan = Next(t);
            UpdateLevels(index);
            continue;

        case 0xF3:
            t.fine = int8_t(Next(t));
            continue;

        case 0xF4:
            t.bend_range = Next(t);
            UpdatePitch(index);
            continue;

        case 0xF5:
            t.bend = int8_t(Next(t));
            UpdatePitch(index);
            continue;

        case 0xF6:
            t.wait = Next(t);
            next = At::kWait;
            continue;

        case 0xF7:
            next = At::kNoteEnd;
            continue;

        case 0xF8:
            {
                const int lo = Next(t);
                const uint16_t jump = uint16_t(lo | (Next(t) << 8));
                const uint16_t to = uint16_t(t.at + jump);
                if (to <= event_at)
                {
                    goto_frames_[size_t(index)].push_back(frame_);
                }

                t.at = to;
                continue;
            }

        case 0xF9:
            t.tempo = Next(t);
            continue;

        case 0xFA:
            // B85A's driver keeps the byte as the channel's priority for sound effects. It's a note's length in A88E
            // and A84J.
            if (info_.version != Version::kHamSports)
            {
                break;
            }

            Next(t);
            continue;

        case 0xFF:
            if (t.flags & flag::kNote)
            {
                NoteOff(index);
            }

            t.flags = 0;
            return;

        default:
            break;
        }

        // A note: its length, then its key with bit 7 set if a rest follows. A length of 0 ties the note to the next.
        // In B85A and A84J, the track's volume follows the key. A84J's driver works the levels out at once. B85A's
        // driver does so only where a note starts, not where a note goes on from a tie.
        const uint8_t length = b;
        const uint8_t k = Next(t);
        t.key = k & 0x7F;
        if (info_.version != Version::kSuperstarSaga)
        {
            t.volume = Next(t);
        }

        if (info_.version == Version::kHamtaro4)
        {
            UpdateLevels(index);
        }

        if (t.flags & flag::kTie)
        {
            UpdatePitch(index);
            t.flags &= uint16_t(~flag::kTie);
        }
        else
        {
            if (t.flags & flag::kLevelLfo)
            {
                t.flags &= uint16_t(~(flag::kLevelLfoOn | flag::kLevelLfoDown));
                t.level_lfo.count = t.level_lfo.delay;
            }

            if (t.flags & flag::kPitchLfo)
            {
                t.flags &= uint16_t(~(flag::kPitchLfoOn | flag::kPitchLfoDown));
                t.pitch_lfo.count = t.pitch_lfo.delay;
            }

            if (info_.version == Version::kHamSports)
            {
                UpdateLevels(index);
            }

            UpdatePitch(index);
            NoteOn(index, t.data + event_at);
            t.flags |= flag::kNote;
        }

        if (length == 0)
        {
            t.flags |= flag::kTie;
            if (k & 0x80)
            {
                KeepRead(index, true, t.data + t.at);
                t.wait = Next(t);
                next = At::kWait;
            }

            continue;
        }

        if (k & 0x80)
        {
            t.flags |= flag::kRests;
        }

        t.wait = length;
        next = At::kWait;
    }

    warnings_.push_back("track " + std::to_string(index) + " runs commands without end");
    t.flags = 0;
}

void Sequencer::Record(int index)
{
    if (open_[size_t(index)] < 0)
    {
        return;
    }

    const Note& n = notes_[size_t(open_[size_t(index)])];
    const TrackState& t = tracks_[size_t(index)];
    Change c{index, frame_, 0, 0, 0};
    if (index < kSampleTracks)
    {
        // A note that its track releases on the frame it starts plays through the release, from the level that the
        // channel had. The SoundFont's envelope can't follow that. The note's levels take the envelope's level in.
        const SampleChannel& s = samples_[t.channel & 7];
        const double envelope = n.released == n.on ? s.level / kFullEnvelope : 1;
        c.pitch = (s.sample & 0x8000) ? 0 : (int(s.pitch) - n.key * 256) / 256.0;
        c.left = s.left * kFullSampleLevel / 127 * envelope;
        c.right = s.right * kFullSampleLevel / 127 * envelope;
    }
    else
    {
        // A square and the noise play at the hardware's level. The wave plays at the share of 15 that NR32 sets.
        // A square and the wave play the frequency setting that the driver keeps, from its pitch table (0x0819A96C).
        // The table truncates 131072 over each key's frequency, and a key below 36 gets the setting 0: 64 Hz on a
        // square. The pitch is the bend from the note's key to that setting. The wave plays an octave below a square,
        // and so does its sample.
        const int h = t.channel & 3;
        const PsgChannel& p = psg_[size_t(h)];
        const double key_hz = KeyHz(n.key);
        c.pitch = h == kNoiseChannel ? 0 : 12 * std::log2(SquareHz(std::min<int>(p.frequency, 2047)) / key_hz);
        const double volume =
            h == kWaveChannel ? 15 * WaveShare(registers_.control[kWaveChannel]) : psg_levels_[size_t(h)];
        c.left = p.left ? volume * kFullSampleLevel / 15 : 0;
        c.right = p.right ? volume * kFullSampleLevel / 15 : 0;
    }

    const Change& last = last_[size_t(index)];
    if (c.pitch != last.pitch || c.left != last.left || c.right != last.right)
    {
        changes_.push_back(c);
        last_[size_t(index)] = c;
    }
}

// Runs the players (0x0819B864) for the music's player alone, the PSG's output, then the mixer.
void Sequencer::Step()
{
    for (int index = 0; index < kTracks; index++)
    {
        TrackState& t = tracks_[size_t(index)];
        if (!(t.flags & flag::kUsed))
        {
            continue;
        }

        if (t.flags & flag::kMuted)
        {
            // The channel comes back (the restore handler, then the note-on if a note sounds). The restore handler of a
            // PSG channel uses the squares' pitch routine, for the noise too. A84J's driver doesn't start the note
            // again. B85A's driver turns the channel off first. The model restores a channel only when a song starts,
            // and nothing sounds on it then.
            t.flags &= uint16_t(~flag::kMuted);
            UpdatePitch(index, true);
            UpdateLevels(index);
            if ((t.flags & flag::kNote) && info_.version != Version::kHamtaro4)
            {
                NoteOn(index);
            }
        }

        StepTrack(index);
    }

    RunPsg();
    RunEnvelopes();
    for (int index = 0; index < kTracks; index++)
    {
        if (open_[size_t(index)] >= 0)
        {
            const int h = tracks_[size_t(index)].channel & 3;
            if (index < kSampleTracks && samples_[size_t(index)].status == 0)
            {
                // A note that its track released on the frame it started and whose channel stopped on that frame
                // never sounded: the channel's level was at most the release's step.
                const Note& n = notes_[size_t(open_[size_t(index)])];
                EndNote(index, n.on == frame_ && n.released == frame_ ? frame_ : frame_ + 1);
                LetGo(index, frame_ + 1);
            }
            else if (index >= kSampleTracks && h != kWaveChannel && !PsgSounds(h))
            {
                EndNote(index, frame_);
            }
        }

        Record(index);
    }

    frame_++;
}

} // namespace agblib::sequence_export::alphadream
