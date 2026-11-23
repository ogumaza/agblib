// SPDX-License-Identifier: MIT

// The simulation of a Rare tune, and the ticks of its frames. Only convert.cpp and convert_notes.h include this
// header. Its definitions therefore keep internal linkage.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "convert_internal.h"

namespace agblib::sequence_export::rare
{
namespace
{

// Maximum conversion duration: one hour, measured in frames.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// Plans the conversion of a tune from its tracks (see PlanSong()).
SongPlan PlanTune(const Rom& rom, const DriverInfo& info, const TuneHeader& header, int loops)
{
    std::vector<TrackLayout> layouts;
    for (uint32_t address : header.tracks)
    {
        layouts.push_back(ScanTrack(rom, address, info.format));
    }

    return PlanSong(layouts, loops, LoopStart::kFirstPass);
}

// A clock that converts frame start times to ticks, following the tempo.
class TickClock
{
public:
    explicit TickClock(uint32_t ticks_per_quarter) : per_quarter_(ticks_per_quarter)
    {
    }

    // Returns the tick at which frame `frame` starts.
    uint64_t FrameTick(uint32_t frame) const
    {
        const int64_t elapsed = int64_t(frame) * kFrameTime - time_;
        if (tempo_ == 0 || elapsed <= 0)
        {
            return tick_;
        }

        return tick_ + uint64_t(elapsed) * per_quarter_ / (uint64_t(tempo_) * 256);
    }

    // Returns true once a tempo has been set.
    bool Known() const
    {
        return tempo_ != 0;
    }

    // Returns the time of `tick`, in 1/256 microseconds at the driver's speed, at the tempo now set.
    int64_t TickTime(uint64_t tick) const
    {
        return time_ + int64_t((tick - std::min(tick, tick_)) * tempo_ * 256 / per_quarter_);
    }

    // Starts a tempo of `tempo` microseconds a quarter note at `tick`.
    void SetTempo(uint64_t tick, uint32_t tempo)
    {
        if (tempo_ != 0 && tick > tick_)
        {
            time_ += int64_t((tick - tick_) * tempo_ * 256 / per_quarter_);
        }

        tick_ = std::max(tick_, tick);
        tempo_ = tempo;
    }

private:
    uint32_t per_quarter_;
    uint64_t tick_ = 0; // the start of the tempo's segment
    int64_t time_ = 0;  // its time, in 1/256 microseconds
    uint32_t tempo_ = 0;
};

// Returns the tempo changes in `actions` as (tick, tempo), in the order the model runs them. The driver keeps the tempo
// it set last. A change can run after one with a later tick when two tracks change the tempo within about a frame. Such
// a change starts at that later tick as in TickClock.
std::vector<std::pair<uint64_t, uint32_t>> TempoChanges(const std::vector<Action>& actions)
{
    std::vector<std::pair<uint64_t, uint32_t>> tempos;
    uint64_t at = 0;
    for (const Action& a : actions)
    {
        if (a.kind == Action::kTempo)
        {
            at = std::max(at, a.tick);
            tempos.emplace_back(at, a.value);
        }
    }

    return tempos;
}

// Returns the tick at which each frame from 0 to `frames` starts, as TickClock gives it, following the tempo changes in
// `actions` in the order of their ticks. The tempo track can play a tick a frame later than other tracks. A tempo
// change can therefore come in a frame that starts after its tick.
std::vector<uint64_t> FrameStarts(const std::vector<Action>& actions, uint32_t per_quarter, uint32_t frames)
{
    const std::vector<std::pair<uint64_t, uint32_t>> tempos = TempoChanges(actions);
    TickClock clock(per_quarter);
    std::vector<uint64_t> starts;
    size_t next = 0;
    for (uint32_t f = 0; f <= frames; f++)
    {
        while (next < tempos.size() &&
               (!clock.Known() || clock.TickTime(tempos[next].first) <= int64_t(f) * kFrameTime))
        {
            clock.SetTempo(tempos[next].first, tempos[next].second);
            next++;
        }

        starts.push_back(clock.FrameTick(f));
    }

    return starts;
}

// Returns the tick at which the first frame that reaches `tick` starts.
uint64_t OnFrame(const std::vector<uint64_t>& starts, uint64_t tick)
{
    const auto it = std::lower_bound(starts.begin(), starts.end(), tick);
    return it == starts.end() ? starts.back() : *it;
}

// Returns the tick at which the frame that plays `tick` starts: the last frame that starts at or before it as on every
// track but the tempo track. `starts` contains what FrameStarts() gives for `actions`.
uint64_t PlayingFrame(const std::vector<Action>& actions, uint32_t per_quarter, const std::vector<uint64_t>& starts,
                      uint64_t tick)
{
    // The frame that plays the tick is the one its time falls in, with the tempo changes up to it as FrameStarts()
    // follows them.
    TickClock clock(per_quarter);
    for (const auto& [at, tempo] : TempoChanges(actions))
    {
        if (clock.Known() && at > tick)
        {
            break;
        }

        clock.SetTempo(at, tempo);
    }

    const uint64_t frame = uint64_t(clock.TickTime(tick) / kFrameTime);
    return starts[size_t(std::min<uint64_t>(frame, starts.size() - 1))];
}

// Follows which keys the mixer leaves out, frame by frame. A key is muted from a frame in which the mixer leaves out
// every voice that plays it, at the voice limit or at a level of 0. The key plays again from a frame in which the mixer
// takes any of them. A frame without voices of the key changes nothing.
class MuteTracker
{
public:
    // Adds the mute changes of `frame` to `changes`, at `tick`. `seq` has just run the frame. The keys come in the
    // order of the slots that first play them in the frame.
    void Update(const Sequencer& seq, uint32_t frame, uint64_t tick, std::vector<MuteChange>& changes);

private:
    // The keys of every channel as channel * 128 + key.
    static constexpr size_t kKeys = kChannels * 128;

    std::vector<bool> muted_ = std::vector<bool>(kKeys, false);

    // In the current frame, -1 for a key without voices, 0 where the mixer leaves out every voice that plays the key,
    // and 1 where the mixer takes any of them.
    std::vector<int8_t> heard_ = std::vector<int8_t>(kKeys, -1);

    std::vector<int> keys_; // the keys that the frame's voices play, in slot order
};

void MuteTracker::Update(const Sequencer& seq, uint32_t frame, uint64_t tick, std::vector<MuteChange>& changes)
{
    for (int i = 0; i < seq.SlotCount(); i++)
    {
        const Slot& s = seq.GetSlot(i);
        if (s.mix == MixResult::kIdle)
        {
            continue;
        }

        const int key = s.channel * 128 + (s.key & 0x7F);
        if (heard_[size_t(key)] < 0)
        {
            keys_.push_back(key);
        }

        heard_[size_t(key)] = std::max<int8_t>(heard_[size_t(key)], s.mix == MixResult::kMixed ? 1 : 0);
    }

    for (int key : keys_)
    {
        const bool mute = heard_[size_t(key)] == 0;
        if (mute != muted_[size_t(key)])
        {
            muted_[size_t(key)] = mute;
            changes.push_back({tick, key / 128, key % 128, mute, frame});
        }

        heard_[size_t(key)] = -1;
    }

    keys_.clear();
}

// Moves each action but a tempo change, and each pitch and mute change, to the start of the frame that the driver plays
// it in. `frames` is the number of frames that the model ran, with `slot_count` note slots. Also leaves out the note
// ons at or after the end.
void PutOnFrames(Simulation& sim, const SongPlan& plan, uint32_t per_quarter, uint32_t frames, int slot_count)
{
    // The notes at or after the end start the loop's next pass. They can come in the frame that plays the end, a frame
    // that starts before the end. Like the default timing, frame timing leaves them out. It also leaves out the
    // envelope settings that their phases take. The conversion would otherwise give those settings to the note before
    // them in their slot.
    const uint64_t end = sim.cut_off ? std::min(plan.end, *sim.cut_off) : plan.end;
    std::vector<bool> slot_past_end(size_t(slot_count), false);
    std::vector<Action> kept;
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            slot_past_end[size_t(a.slot)] = a.tick >= end;
        }

        const bool note_past_end = (a.kind == Action::kNoteOn || a.kind == Action::kNoteDropped) && a.tick >= end;
        if (!note_past_end && !(a.kind == Action::kEnvelope && slot_past_end[size_t(a.slot)]))
        {
            kept.push_back(a);
        }
    }

    sim.actions = std::move(kept);

    sim.frame_starts = FrameStarts(sim.actions, per_quarter, frames);
    for (Action& a : sim.actions)
    {
        if (a.kind != Action::kTempo)
        {
            a.tick = sim.frame_starts[std::min(a.frame, frames)];
        }
    }

    for (PitchChange& p : sim.pitches)
    {
        p.tick = sim.frame_starts[std::min(p.frame, frames)];
    }

    for (MuteChange& m : sim.mutes)
    {
        m.tick = sim.frame_starts[std::min(m.frame, frames)];
    }
}

// Leaves out everything that the channels in `silenced` do, but the tempo changes. The game contains those channels at
// a level of 0. The mixer then leaves out their notes. The MIDI file has none of their events.
void LeaveOutChannels(Simulation& sim, uint16_t silenced)
{
    auto silent = [silenced](int channel)
    {
        return (silenced >> channel & 1) != 0;
    };

    std::erase_if(sim.actions, [&](const Action& a) { return a.kind != Action::kTempo && silent(a.channel); });
    std::erase_if(sim.pitches, [&](const PitchChange& p) { return silent(p.channel); });
    std::erase_if(sim.mutes, [&](const MuteChange& m) { return silent(m.channel); });
}

// Runs the model until the plan's end or until the driver has stopped the tune, for an hour at most. The channels in
// `silenced` play at a level of 0. With `frame_timing`, each action but a tempo change, and each pitch and mute change,
// goes at the start of the frame the driver plays it in. Frame timing also leaves out the note ons at or after the end.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, uint16_t silenced, const TuneHeader& header,
                    const SongPlan& plan, bool frame_timing, bool stretches = false)
{
    Simulation sim;
    Sequencer seq(rom, info, song, silenced);
    seq.RecordStretches(stretches);
    TickClock clock(header.ticks_per_quarter);
    std::array<int64_t, kChannels> last = {};
    MuteTracker mute_tracker;
    uint32_t frames = 0;
    bool finished = false; // the tune reached the plan's end, or the driver stopped it
    for (uint32_t f = 0; f < kMaxFrames; f++)
    {
        // A pitch change that a bend or program change makes, without vibrato, belongs at that command's tick. Vibrato
        // changes the pitch at the start of each frame.
        const uint64_t frame_tick = clock.FrameTick(f);
        std::array<int64_t, kChannels> command_tick;
        command_tick.fill(-1);
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kTempo)
            {
                clock.SetTempo(a.tick, a.value);
            }

            if (a.kind == Action::kBend || a.kind == Action::kProgram)
            {
                command_tick[a.channel] = int64_t(a.tick);
            }

            sim.actions.push_back(a);
        }

        for (int ch = 0; ch < kChannels; ch++)
        {
            const int64_t offset = seq.PitchOffset(ch);
            if (offset == last[size_t(ch)])
            {
                continue;
            }

            last[size_t(ch)] = offset;
            const bool vibrato = seq.Channel(ch).modulation != 0;
            const int64_t at = command_tick[size_t(ch)];
            sim.pitches.push_back({!vibrato && at >= 0 ? uint64_t(at) : frame_tick, ch, offset, f});
        }

        mute_tracker.Update(seq, f, frame_tick, sim.mutes);

        // Stop once every track has read up to the end, and the frames have reached it. A track counts the ticks of a
        // delay as it starts it. Its count therefore reaches the end before the frames do.
        frames = f + 1;
        bool done = clock.Known() ? clock.FrameTick(f + 1) >= plan.end : true;
        for (int t = 0; t < seq.TrackCount() && done; t++)
        {
            done = seq.TrackDone(t) || seq.TrackTick(t) >= plan.end;
        }

        if (done || seq.Ended())
        {
            finished = true;
            break;
        }
    }

    if (!finished && frames == kMaxFrames)
    {
        sim.warnings.push_back("the tune was cut off after an hour");
        sim.cut_off = clock.FrameTick(frames);
    }

    sim.warnings.insert(sim.warnings.end(), seq.Warnings().begin(), seq.Warnings().end());

    for (int ch = 0; ch < kChannels; ch++)
    {
        sim.end_envelopes[size_t(ch)] = seq.Channel(ch).envelope;
    }

    sim.stretches = seq.Stretches();

    LeaveOutChannels(sim, silenced);
    if (frame_timing)
    {
        PutOnFrames(sim, plan, header.ticks_per_quarter, frames, seq.SlotCount());
    }

    return sim;
}

} // namespace
} // namespace agblib::sequence_export::rare
