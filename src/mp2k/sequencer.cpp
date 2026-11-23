// SPDX-License-Identifier: MIT

#include "agblib/mp2k/sequencer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/sound_registers.h"
#include "agblib/mp2k/commands.h"
#include "agblib/mp2k/driver.h"
#include "agblib/mp2k/song.h"
#include "sequencer_shared.h"

namespace agblib::mp2k
{
using agblib::agb::Rom;
using ntdlib::commands::Kind;

namespace
{

// 2^31 * 2^(n/12), rounded: a semitone's steps in the driver's table of sample rates, an octave below key 180.
constexpr uint32_t kFreqTable[12] = {2147483648u, 2275179671u, 2410468894u, 2553802834u, 2705659852u, 2866546760u,
                                     3037000500u, 3217589947u, 3408917802u, 3611622603u, 3826380858u, 4053909305u};

// The PSG's periods for the notes C2 to B2 at A4 = 440 Hz, negated: -131072 / f, rounded. A frequency setting is 2048
// less the period.
constexpr int16_t kPsgFreqTable[12] = {-2004, -1891, -1785, -1685, -1591, -1501,
                                       -1417, -1337, -1262, -1192, -1125, -1062};

// Returns (a * b) >> 32.
uint32_t MulHigh(uint32_t a, uint32_t b)
{
    return uint32_t((uint64_t(a) * b) >> 32);
}

// Returns a key's entry in the driver's table of sample rates: the semitone in the low 4 bits, and the octaves below
// keys 168-179 in the high 4.
uint32_t ScaleEntry(int key)
{
    return uint32_t(((14 - key / 12) << 4) | (key % 12));
}

} // namespace

uint32_t KeyToFrequency(uint32_t frequency, int key, int fine)
{
    key &= 0xFF;
    uint32_t fraction = uint32_t(fine & 0xFF) << 24;
    if (key > 178)
    {
        key = 178;
        fraction = 255u << 24;
    }

    const uint32_t low = kFreqTable[ScaleEntry(key) & 0xF] >> (ScaleEntry(key) >> 4);
    const uint32_t high = kFreqTable[ScaleEntry(key + 1) & 0xF] >> (ScaleEntry(key + 1) >> 4);
    return MulHigh(frequency, low + MulHigh(high - low, fraction));
}

uint32_t CamelotKeyToFrequency(uint32_t frequency, int key, int fine)
{
    key &= 0xFF;
    uint32_t fraction = uint32_t(fine & 0xFF) << 24;
    if (key > 178)
    {
        key = 178;
        fraction = 255u << 24;
    }

    // The table contains each semitone's step to the next. From B, that is the step to 2^32, and it wraps to the same
    // in 32 bits.
    const uint32_t semitone = ScaleEntry(key) & 0xF;
    const uint32_t octaves = ScaleEntry(key) >> 4;
    const uint32_t step = (semitone == 11 ? 0 : kFreqTable[semitone + 1]) - kFreqTable[semitone];
    return MulHigh(frequency, (kFreqTable[semitone] >> octaves) + MulHigh(step >> octaves, fraction));
}

uint32_t SampleFrequency(const DriverInfo& info, uint32_t frequency, int key, int fine)
{
    return info.camelot_sequencer ? CamelotKeyToFrequency(frequency, key, fine) : KeyToFrequency(frequency, key, fine);
}

uint32_t KeyToPsgFrequency(int channel, int key, int fine)
{
    key &= 0xFF;
    fine &= 0xFF;

    // The noise settings go up a step a key from key 21: through ratios 7 to 4 of each clock shift from 13 down to 0,
    // then ratios 3 to 0 of shift 0 at keys 77 to 80.
    if (channel == 4)
    {
        const int index = key <= 20 ? 0 : std::min(key - 21, 59);
        return index < 56 ? uint32_t(((13 - index / 4) << 4) | (7 - index % 4)) : uint32_t(59 - index);
    }

    if (key <= 35)
    {
        fine = 0;
        key = 0;
    }
    else
    {
        key -= 36;
        if (key > 130)
        {
            key = 130;
            fine = 255;
        }
    }

    const int low = kPsgFreqTable[key % 12] >> (key / 12);
    const int high = kPsgFreqTable[(key + 1) % 12] >> ((key + 1) / 12);
    return uint32_t(low + ((fine * (high - low)) >> 8) + 2048);
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song)
    : rom_(rom), info_(info), direct_channels_(SongChannels(rom, info, song))
{
    // The buffer has room for as many frames' parts as fit in it. A song whose header sets the reverb sets it for the
    // mixer. The game's code can set it before the start too. m4aMPlayVolumeControl gives each track a quarter of the
    // player's volume.
    if (info.samples_per_frame > 0)
    {
        dma_period_ = uint8_t(std::min<uint32_t>(kPcmBufferSize / uint32_t(info.samples_per_frame), 255));
    }

    reverb_ = uint8_t(info.reverb);
    vol_x_ = uint8_t(SongVolume(info, song) / 4);
    if (!ReadSong(rom, info, song, header_))
    {
        return;
    }

    reverb_ = uint8_t(SongReverb(info, song, header_));

    // The song's tracks go on the player's as far as it has room for them. An unused player has no room.
    const int count = PlayedTracks(rom, info, song, header_);
    valid_ = count > 0;
    tracks_.resize(size_t(count));
    for (int t = 0; t < count; t++)
    {
        tracks_[size_t(t)].flags = kFlagExist | kFlagStart;
        tracks_[size_t(t)].position = header_.tracks[size_t(t)];
    }

    if (count < header_.track_count)
    {
        Warn("the song has " + std::to_string(header_.track_count) + " tracks, and its music player plays only " +
             std::to_string(count));
    }

    priority_ = header_.priority;

    // The SDK's init gives the PSG's channels an envelope of 0 and restarts them, turns the wave channel off, and sets
    // the master volumes and the outputs. The DAC's resolution goes in SOUNDBIAS's high bits. The BIOS leaves the bias
    // at 0x200.
    auto set = [this](uint32_t address, uint8_t value)
    {
        sound_registers_[address - agb::kRegSound1CntL] = value;
    };

    for (int n = 0; n < 4; n++)
    {
        if (n != 2)
        {
            set(kPsgRegisters[n].nrx2, 0x08);
            set(kPsgRegisters[n].nrx4, 0x80);
        }
    }

    set(agb::kRegNr50, 0x77);
    set(agb::kRegSoundCntH, 0x0E);
    set(agb::kRegSoundCntHHigh, info_.mono ? 0x0B : 0xA9);
    set(agb::kRegSoundCntX, 0x8F);
    set(agb::kRegSoundBias + 1, uint8_t(info_.dac_bits | 0x02));

    for (int n = 0; n < 4; n++)
    {
        Channel& c = channels_[size_t(kFirstPsgChannel + n)];
        c.type = uint8_t(n + 1);
        c.part = uint8_t(n + 1);
        c.pan_mask = uint8_t(0x11 << n);
    }
}

void Sequencer::GrowChannels(bool on)
{
    growing_ = on;
    if (on)
    {
        channels_.reserve(kMostChannels);
        owners_.reserve(kMostChannels);
        enhanced_channels_.reserve(kMostChannels);
    }
}

int Sequencer::GrownChannel(int part)
{
    // A PSG channel's part is in one unit. A new one takes the next unit that contains none of the part's channels.
    int units = 0;
    for (int c = kChannelCount; c < ChannelCount(); c++)
    {
        const Channel& ch = channels_[size_t(c)];
        if (ch.part == part)
        {
            if (!(ch.status & kStatusOn))
            {
                return c;
            }

            units++;
        }
    }
    if (ChannelCount() >= kMostChannels || (part && units + 1 >= agb::kMostPsgUnits))
    {
        return -1;
    }

    const int index = ChannelCount();
    Channel& ch = channels_.emplace_back();
    owners_.push_back(-1);
    enhanced_channels_.emplace_back();
    ch.part = uint8_t(part);
    if (part)
    {
        // The new unit starts as the SDK's init leaves the console's PSG: each channel but the wave channel restarted
        // with an envelope of 0.
        ch.type = uint8_t(part);
        ch.unit = uint16_t(units + 1);
        ch.pan_mask = uint8_t(0x11 << (part - 1));
        if (size_t(ch.unit) > grown_registers_.size())
        {
            grown_registers_.emplace_back();
            for (int n = 0; n < 4; n++)
            {
                if (n != 2)
                {
                    WriteSound(UnitAddress(ch.unit, kPsgRegisters[n].nrx2), 1, 0x08);
                    WriteSound(UnitAddress(ch.unit, kPsgRegisters[n].nrx4), 1, 0x80);
                }
            }
        }
    }

    return index;
}

const std::vector<Action>& Sequencer::Step()
{
    actions_.clear();
    sound_writes_.clear();

    // m4aSoundVSync counts the frames down to the sound DMA's next start of the buffer.
    dma_counter_ = dma_counter_ > 1 ? uint8_t(dma_counter_ - 1) : dma_period_;

    // Each 150 of the tempo is a tick. The tracks that existed at the start of a tick count as playing. The player
    // therefore stops in the tick after its last track ended, and then skips the update of the channels.
    if (valid_ && !(status_ & kPlayerStopped))
    {
        tempo_c_ = uint16_t(tempo_c_ + tempo_i_);
        bool stopped = false;
        while (tempo_c_ >= kTickCounter)
        {
            if (clock_ == restate_from_)
            {
                for (Track& track : tracks_)
                {
                    track.restate = kSetVolume | kSetPan | kSetPitch;
                }
            }

            uint32_t playing = 0;
            for (int t = 0; t < TrackCount(); t++)
            {
                if (tracks_[size_t(t)].flags & kFlagExist)
                {
                    playing |= 1u << t;
                    RunTrack(t);
                }
            }

            clock_++;
            if (!playing)
            {
                status_ = kPlayerStopped;
                stopped = true;
                break;
            }

            status_ = playing;
            tempo_c_ = uint16_t(tempo_c_ - kTickCounter);
        }

        if (!stopped)
        {
            UpdateChannels();
        }
    }

    RunPsg();
    RunDirect();
    frame_++;

    return actions_;
}

bool Sequencer::Ended() const
{
    if (!TracksEnded() && valid_)
    {
        return false;
    }

    for (int c = 0; c < ChannelCount(); c++)
    {
        if ((c < direct_channels_ || c >= kFirstPsgChannel) && (channels_[size_t(c)].status & kStatusOn))
        {
            return false;
        }
    }

    return true;
}

void Sequencer::RunTrack(int t)
{
    Track& track = tracks_[size_t(t)];

    // Count down the notes' lengths, and take channels that have stopped off the track's list.
    for (int c = track.chan; c >= 0;)
    {
        Channel& ch = channels_[size_t(c)];
        const int next = ch.next;
        if (!(ch.status & kStatusOn))
        {
            UnlinkChannel(c);
        }
        else if (ch.gate && --ch.gate == 0)
        {
            ReleaseNote(t, c);
        }

        c = next;
    }

    if (track.flags & kFlagStart)
    {
        StartTrack(track);
    }

    // The Metroid games' sequencer counts a wait down and moves the LFO on before it runs the commands that are due. A
    // tick that runs commands moves the LFO on only if the track was waiting at its start.
    if (info_.metroid_sequencer && track.wait)
    {
        track.wait--;
        RunMetroidLfo(track);
    }

    // Run the commands that are due, up to the next wait. A track that ends doesn't move its LFO on.
    for (int commands = 0; track.wait == 0; commands++)
    {
        if (commands == kCommandBudget)
        {
            throw Hang(t, track.position);
        }

        if (!RunCommand(t))
        {
            return;
        }
    }

    if (info_.metroid_sequencer)
    {
        ReportTrack(t);
        return;
    }

    track.wait--;

    // The LFO's shape is a triangle wave from -64 to 64, scaled by the depth. Camelot's sequencer counts the LFO's
    // delay down before it looks at the speed. It therefore counts the delay down at a speed of 0 too.
    if ((track.lfo_speed || info_.camelot_sequencer) && track.mod)
    {
        if (track.lfo_delay_c)
        {
            track.lfo_delay_c--;
        }
        else if (track.lfo_speed)
        {
            const uint32_t phase = uint32_t(track.lfo_speed_c) + track.lfo_speed;
            track.lfo_speed_c = uint8_t(phase);
            const int32_t shape = int8_t(uint8_t(phase - 64)) >= 0 ? 128 - int32_t(phase) : int8_t(uint8_t(phase));
            const int32_t depth = (int32_t(track.mod) * shape) >> 6;
            if (uint8_t(depth) != uint8_t(track.mod_m))
            {
                track.mod_m = int8_t(uint8_t(depth));
                track.flags |= track.mod_t == 0 ? kFlagPitChange : kFlagVolChange;
            }
        }
    }

    ReportTrack(t);
}

void Sequencer::RunMetroidLfo(Track& track)
{
    // The delay counts down at any speed and depth. The LFO moves on only while the track's newest note sounds. The
    // swing is half as wide as in the SDK's library.
    if (track.lfo_delay_c)
    {
        track.lfo_delay_c--;
        return;
    }

    if (!track.lfo_speed || !track.mod || track.chan < 0 || !(channels_[size_t(track.chan)].status & kStatusOn))
    {
        return;
    }

    // The sequencer leaves the swing as it is when the triangle wave's new value, before the depth scales it, equals
    // the old swing (0x080016E0 in Metroid Fusion).
    const uint32_t phase = uint32_t(track.lfo_speed_c) + track.lfo_speed;
    track.lfo_speed_c = uint8_t(phase);
    const int32_t shape = int8_t(uint8_t(phase - 64)) >= 0 ? 128 - int32_t(uint8_t(phase)) : int8_t(uint8_t(phase));
    if (shape == track.mod_m)
    {
        return;
    }

    track.mod_m = int8_t(uint8_t(((int32_t(track.mod) + 1) * shape) >> 7));
    track.flags |= track.mod_t == 0 ? kFlagPitChange : kFlagVolChange;
}

bool Sequencer::RunCommand(int t)
{
    Track& track = tracks_[size_t(t)];
    if (!rom_.Contains(track.position))
    {
        Warn("track " + std::to_string(t) + " runs past the end of the ROM, so it stops there");
        EndTrack(t);
        return false;
    }

    // The decoder reads the command as the driver does, with the running status and the last key and velocity.
    const uint32_t address = track.position;
    const PlayedCommand played =
        ReadPlayedCommand([this](uint32_t at) { return rom_.U8(at); }, address,
                          RunningState{track.running, track.key, track.velocity}, NoteArguments(info_));
    if (!played.valid)
    {
        Warn("track " + std::to_string(t) +
             " has a byte below 0x80 before any command it could repeat, so it stops there");
        EndTrack(t);
        return false;
    }

    const ntdlib::commands::Command& command = played.command;
    const int64_t value = command.value.value;
    const bool waits_again = command.kind == Kind::kWait && track.timer != 0;
    if (record_commands_ && !waits_again)
    {
        Action a;
        a.kind = Action::kCommand;
        a.track = uint8_t(t);
        a.command = address;
        a.next = played.next;
        a.value = int32_t(value);
        a.command_kind = command.kind;
        AddAction(a);
    }

    track.running = played.after.running;
    track.key = played.after.key;
    track.velocity = played.after.velocity;
    track.position = played.next;
    switch (command.kind)
    {
    case Kind::kNote:
        track.gate = uint8_t(value);
        Note(t, address);
        return true;

    case Kind::kNoteOff:
        EndTie(t);
        break;

    case Kind::kWait:
        if (command.encoding.opcode == (kCmdXcmd << 8 | kXcmdWait))
        {
            // The extended wait runs again on each tick until its count runs out. The driver goes back 2 bytes from
            // its count, to the byte before the extended command's number.
            if (track.timer < value)
            {
                track.timer++;
                track.position = played.extended - 1;
                track.wait = 1;
            }
            else
            {
                track.timer = 0;
            }
            break;
        }

        track.wait = uint8_t(value);
        return true;

    case Kind::kJump:
        track.position = command.target;
        break;

    case Kind::kCall:
        // A pattern can call another, three deep. One more ends the track.
        if (track.pattern_level < 3)
        {
            track.stack[track.pattern_level++] = played.next;
            track.position = command.target;
        }
        else
        {
            EndTrack(t);
        }
        break;

    case Kind::kReturn:
        if (track.pattern_level)
        {
            track.position = track.stack[--track.pattern_level];
        }
        break;

    case Kind::kRepeat:
        // A count of 0 repeats for ever. The track has one count, for all of its repeats.
        if (value == 0 || ++track.repeats < value)
        {
            track.position = command.target;
        }
        else
        {
            track.repeats = 0;
        }
        break;

    case Kind::kSetVariable:
    case Kind::kAddVariable:
    case Kind::kSubtractVariable:
    case Kind::kJumpIfEqual:
    case Kind::kJumpIfNotEqual:
    case Kind::kJumpIfGreater:
    case Kind::kJumpIfGreaterOrEqual:
    case Kind::kJumpIfLessOrEqual:
    case Kind::kJumpIfLess:
        MemAcc(track, command);
        break;

    case Kind::kOther:
        // MEMACC with an operation past 17 does nothing.
        break;

    case Kind::kPriority:
        track.priority = uint8_t(value);
        break;

    case Kind::kTempo:
        {
            // The frame's progress has already been counted at the old tempo, and so has any of it left over for the
            // next tick.
            Action a;
            a.kind = Action::kTempo;
            a.track = uint8_t(t);
            a.old_tempo = tempo_i_;
            a.counter = tempo_c_ - kTickCounter;
            tempo_d_ = uint16_t(value * 2);
            tempo_i_ = uint16_t((uint32_t(tempo_d_) * tempo_u_) >> 8);
            a.value = tempo_i_;
            AddAction(a);
            break;
        }

    case Kind::kTranspose:
        track.key_shift = int8_t(value);
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case Kind::kProgram:
        {
            // The track keeps a copy of the voice.
            const int voice = int(value);
            ReadVoice(rom_, header_.voices + 12 * uint32_t(voice), track.tone);
            track.voice = voice;
            track.modified = false;
            Action a;
            a.kind = Action::kVoice;
            a.track = uint8_t(t);
            a.value = voice;
            AddAction(a);
            break;
        }

    case Kind::kVolume:
        track.vol = uint8_t(value);
        track.flags |= kFlagVolChange;
        track.sets |= kSetVolume;
        break;

    case Kind::kPan:
        track.pan = int8_t(uint8_t(value - 0x40));
        track.flags |= kFlagVolChange;
        track.sets |= kSetPan;
        break;

    case Kind::kPitchBend:
        track.bend = int8_t(uint8_t(value - 0x40));
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case Kind::kBendRange:
        track.bend_range = uint8_t(value);
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case Kind::kLfoSpeed:
        // The Metroid games' sequencer halves the speed, rounding up, and keeps the LFO's swing.
        track.lfo_speed = info_.metroid_sequencer ? uint8_t((value + 1) >> 1) : uint8_t(value);
        if (!track.lfo_speed && !info_.metroid_sequencer)
        {
            ClearModulation(track);
        }
        break;

    case Kind::kLfoDelay:
        track.lfo_delay = uint8_t(value);
        break;

    case Kind::kLfoDepth:
        track.mod = uint8_t(value);
        if (!track.mod && !info_.metroid_sequencer)
        {
            ClearModulation(track);
        }
        break;

    case Kind::kLfoType:
        if (track.mod_t != uint8_t(value))
        {
            track.mod_t = uint8_t(value);
            track.flags |= kFlagVolPitChange;
        }
        break;

    case Kind::kTune:
        track.tune = int8_t(uint8_t(value - 0x40));
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case Kind::kRegister:
        {
            // The byte goes to the register that many bytes past NR10. A register past the wave RAM is left alone.
            const uint32_t register_address = agb::kRegSound1CntL + command.index;
            if (register_address < agb::kRegFifoA)
            {
                WriteSound(register_address, 1, uint8_t(value));
            }

            Warn("track " + std::to_string(t) + " writes to the sound registers, which the conversion leaves out");
            break;
        }

    case Kind::kSampleWave:
    case Kind::kVoiceType:
    case Kind::kAttack:
    case Kind::kDecay:
    case Kind::kSustain:
    case Kind::kRelease:
    case Kind::kEchoVolume:
    case Kind::kEchoLength:
    case Kind::kPsgLength:
    case Kind::kSweep:
    case Kind::kSampleOffset:
        ExtendedCommand(t, command);
        break;

    default:
        // kCmdFine, the codes that the driver's command table gives its handler, and the extended commands that end
        // the track.
        if (command.encoding.opcode >> 8 == kCmdXcmd && (command.encoding.opcode & 0xFF) >= kXcmdCount)
        {
            Warn("track " + std::to_string(t) + " has an unknown extended command, so it stops there");
        }

        EndTrack(t);
        break;
    }

    return track.flags != 0;
}

} // namespace agblib::mp2k
