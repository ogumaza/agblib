// SPDX-License-Identifier: MIT

// The notes of a Rare tune, the key pressures that mute them, and the summary of a tune. Only convert.cpp includes this
// header. Its definitions therefore keep internal linkage.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include "convert_internal.h"
#include "convert_simulation.h"

namespace agblib::sequence_export::rare
{
namespace
{

NoteCollector::NoteCollector(const Simulation& sim, const SongPlan& plan, uint32_t track_mask, int slot_count)
    : sim_(sim),
      plan_(plan),
      track_mask_(track_mask),
      slot_note_(size_t(slot_count), -1),
      slot_level_(size_t(slot_count), 0),
      slot_last_(size_t(slot_count), -1)
{
    for (auto& keys : owner_)
    {
        keys.fill(-1);
    }

    latest_ = owner_;

    // A track can play a note just before it gives the channel its first program, at the same tick. The game plays the
    // note with whatever instrument the channel had from the tune before. The MIDI file gives the note the program that
    // the channel gets at its tick.
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kProgram)
        {
            first_program_.emplace(std::make_pair(int(a.channel), a.tick), &a);
        }
    }
}

std::vector<Note> NoteCollector::Collect()
{
    for (const Action& a : sim_.actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            NoteOn(a);
        }
        else if (a.kind == Action::kEnvelope)
        {
            TakeSettings(a);
        }
        else if (a.kind == Action::kNoteOff)
        {
            NoteOff(a);
        }
    }

    // The MIDI file releases the notes still playing at the end. Each takes the release that its channel's settings
    // give there. The driver's release would take the same.
    for (size_t i = 0; i < notes_.size(); i++)
    {
        Note& n = notes_[i];
        if (n.open && n.envelope.release >= 0x80)
        {
            n.envelope.release = sim_.end_envelopes[size_t(n.channel)].release;
        }

        Close(int(i), plan_.end);
    }

    return std::move(notes_);
}

void NoteCollector::NoteOn(const Action& a)
{
    Release(a.slot, a.tick, a.frame);
    slot_last_[size_t(a.slot)] = -1;

    const Action* program = ProgramFor(a);
    if (!program || !(track_mask_ >> a.track & 1) || a.tick >= plan_.end)
    {
        return;
    }

    // A key played twice on the same frame is one MIDI note as loud as the two voices together. The driver starts both
    // on the frame.
    int& held = owner_[a.channel][a.a & 0x7F];
    slot_level_[size_t(a.slot)] = a.b + 1;
    if (held >= 0 && notes_[size_t(held)].frame == a.frame)
    {
        Note& n = notes_[size_t(held)];
        n.level += a.b + 1;
        n.velocity = std::min(127, n.level - 1);
        n.voices++;
        slot_note_[size_t(a.slot)] = held;
        slot_last_[size_t(a.slot)] = held;
        return;
    }

    // A player takes a tick's events track by track. So where the older note is in a later track, it ends a tick before
    // the newer one starts. Otherwise its note off would end the newer note too. If the older note started a tick
    // before, the newer one starts a tick late instead.
    uint64_t on = a.tick;
    if (held >= 0 && notes_[size_t(held)].track > a.track)
    {
        on = std::max(on, notes_[size_t(held)].on + 2);
        Close(held, on - 1);
    }
    else if (held >= 0)
    {
        Close(held, on);
    }

    // A note lasts at least a tick, even one that the driver releases in the frame it starts in. Frame timing puts such
    // a release on the note's start tick. So where the key's last note ends after this one's start, this one starts as
    // that one ends. It starts a tick later if that note is in a later track, for the reason above.
    int& previous = latest_[a.channel][a.a & 0x7F];
    if (previous >= 0 && notes_[size_t(previous)].off > on)
    {
        on = notes_[size_t(previous)].off + (notes_[size_t(previous)].track > a.track ? 1 : 0);
    }

    Note n;
    n.track = a.track;
    n.channel = a.channel;
    n.key = a.a & 0x7F;
    n.velocity = a.b;
    n.level = a.b + 1;
    n.program = program == &a ? a.program : program->a;
    n.instrument = program->instrument;
    n.on = on;
    n.frame = a.frame;
    n.command = a.address;
    notes_.push_back(n);
    held = int(notes_.size()) - 1;
    previous = held;
    slot_note_[size_t(a.slot)] = held;
    slot_last_[size_t(a.slot)] = held;
}

void NoteCollector::NoteOff(const Action& a)
{
    // Where a note off releases an older note of the key, the MIDI file has ended that note already. The newer note
    // goes on playing. A note off that releases nothing ends a note that has ended by itself in the driver.
    const int held = owner_[a.channel][a.a & 0x7F];
    if (held >= 0 && a.slot < 0)
    {
        Close(held, std::min(a.tick, plan_.end));
    }
    else if (held >= 0 && slot_note_[size_t(a.slot)] == held)
    {
        Release(a.slot, std::min(a.tick, plan_.end), a.frame);
    }
}

void NoteCollector::TakeSettings(const Action& a)
{
    // Each phase takes the settings that the channel has when the phase starts. Those may differ from the settings at
    // the note's start.
    const int index = slot_last_[size_t(a.slot)];
    if (index >= 0)
    {
        notes_[size_t(index)].envelope.Merge(a.envelope);
    }
}

const Action* NoteCollector::ProgramFor(const Action& a) const
{
    if (a.playable)
    {
        return &a;
    }
    if (a.instrument != 0)
    {
        return nullptr;
    }

    const auto found = first_program_.find(std::make_pair(int(a.channel), a.tick));
    return found != first_program_.end() ? found->second : nullptr;
}

void NoteCollector::Close(int index, uint64_t tick)
{
    // A note lasts at least a tick. Its note off then can't come before its note on.
    Note& n = notes_[size_t(index)];
    if (n.open)
    {
        n.open = false;
        n.off = std::max(tick, n.on + 1);
        owner_[size_t(n.channel)][size_t(n.key)] = -1;
    }
}

void NoteCollector::Release(int slot, uint64_t tick, uint32_t frame)
{
    const int index = slot_note_[size_t(slot)];
    slot_note_[size_t(slot)] = -1;
    if (index < 0)
    {
        return;
    }

    // A voice that ends in the note's first frame sounds for two frames at most. The note then keeps the combined
    // velocity of the remaining voices.
    Note& n = notes_[size_t(index)];
    if (--n.voices == 0)
    {
        Close(index, tick);
    }
    else if (frame == n.frame)
    {
        n.level -= slot_level_[size_t(slot)];
        n.velocity = std::min(127, n.level - 1);
    }
}

// Returns the key pressures that mute the notes that the mixer leaves out, on the channels that `notes` play: 127 from
// the frame in which the mixer leaves out every voice of a key on a channel, and 0 from the frame in which it takes one
// again. A player that jumps back to the loop's start keeps the pressures that the loop's end left. So a key that has
// another pressure at the loop's end gets the pressure from the loop's start again there, unless it changes there.
std::vector<MuteChange> KeyPressures(const Simulation& sim, const SongPlan& plan, const std::vector<Note>& notes)
{
    // The channels that the notes play, and the first note on of each key on each channel in each frame.
    std::array<bool, kChannels> plays = {};
    std::map<std::tuple<int, int, uint32_t>, uint64_t> first_on;
    for (const Note& n : notes)
    {
        plays[size_t(n.channel)] = true;
        uint64_t& on = first_on.try_emplace(std::make_tuple(n.channel, n.key, n.frame), n.on).first->second;
        on = std::min(on, n.on);
    }

    // A note that a track plays a frame late starts before its frame does. A pressure from the frame in which a note of
    // its key starts therefore goes no later than the note on. The note then starts with it. Each key's pressures keep
    // their order.
    std::vector<MuteChange> pressures;
    std::vector<uint64_t> last(kChannels * 128, 0);
    for (MuteChange m : sim.mutes)
    {
        const auto found = first_on.find(std::make_tuple(m.channel, m.key, m.frame));
        uint64_t& previous = last[size_t(m.channel * 128 + m.key)];
        m.tick = std::max(previous, found != first_on.end() ? std::min(m.tick, found->second) : m.tick);
        if (m.tick < plan.end && plays[size_t(m.channel)])
        {
            previous = m.tick;
            pressures.push_back(m);
        }
    }

    if (!plan.loops)
    {
        return pressures;
    }

    // Each key's pressure at the loop's start, before its end and at its end, and whether it changes at the start.
    struct LoopPressure
    {
        bool start = false, before_end = false, end = false, changes_at_start = false;
    };
    std::map<std::pair<int, int>, LoopPressure> keys;
    for (const MuteChange& m : pressures)
    {
        LoopPressure& p = keys[{m.channel, m.key}];
        p.start = m.tick <= plan.loop_start ? m.muted : p.start;
        p.before_end = m.tick < plan.loop_end ? m.muted : p.before_end;
        p.end = m.tick <= plan.loop_end ? m.muted : p.end;
        p.changes_at_start = p.changes_at_start || m.tick == plan.loop_start;
    }

    for (const auto& [key, p] : keys)
    {
        if (!p.changes_at_start && (p.before_end != p.start || p.end != p.start))
        {
            pressures.push_back({plan.loop_start, key.first, key.second, p.start, 0});
        }
    }

    return pressures;
}

// Returns pressure 0 at the start for each key that `notes` play before `pressures` give the key a pressure. A player
// that keeps key pressures from one file to the next would otherwise play the file with the pressures that the file
// before left. With a SoundFont that mutes notes, a key left at 127 would mute the file's notes on it.
std::vector<MuteChange> StartPressures(const std::vector<Note>& notes, const std::vector<MuteChange>& pressures)
{
    // Each key's first note on.
    std::map<std::pair<int, int>, uint64_t> first_on;
    for (const Note& n : notes)
    {
        uint64_t& on = first_on.try_emplace({n.channel, n.key}, n.on).first->second;
        on = std::min(on, n.on);
    }

    // A pressure at or before a key's first note on comes before that note. Such a key gets no pressure at the start.
    for (const MuteChange& m : pressures)
    {
        const auto found = first_on.find({m.channel, m.key});
        if (found != first_on.end() && m.tick <= found->second)
        {
            first_on.erase(found);
        }
    }

    std::vector<MuteChange> start;
    for (const auto& [key, on] : first_on)
    {
        start.push_back({0, key.first, key.second, false, 0});
    }

    return start;
}

// Returns the pitch bend range of each channel, in semitones: wide enough for its largest pitch offset, and its
// instruments' bend range. A bend without vibrato then keeps its value.
std::array<int, kChannels> BendRanges(const Rom& rom, const Simulation& sim)
{
    std::array<int, kChannels> range = {};
    for (const PitchChange& p : sim.pitches)
    {
        const int64_t size = p.offset < 0 ? -p.offset : p.offset;
        range[size_t(p.channel)] = std::max(range[size_t(p.channel)], int((size + kSemitone - 1) / kSemitone));
    }

    // The bends' instrument ranges count for channels with any pitch change.
    std::array<uint32_t, kChannels> instrument = {};
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kProgram)
        {
            instrument[a.channel] = a.instrument;
        }

        if (a.kind == Action::kBend && range[a.channel] > 0)
        {
            Instrument inst;
            if (ReadInstrument(rom, instrument[a.channel], inst))
            {
                range[a.channel] = std::max(range[a.channel], std::clamp(int(inst.bend_range), 0, 127));
            }
        }
    }

    for (int& r : range)
    {
        r = std::min(r, 127);
    }

    return range;
}

// Returns the elapsed seconds at `tick`, using the GBA's frame rate and the tempo changes in `actions`.
double TickSeconds(const std::vector<Action>& actions, uint32_t per_quarter, uint64_t tick)
{
    // Add up the duration of each tempo segment that starts before `tick`.
    double seconds = 0;
    uint64_t at = 0;
    uint32_t tempo = 500000;
    for (const auto& [change, value] : TempoChanges(actions))
    {
        if (change >= tick)
        {
            break;
        }

        seconds += double(change - at) * tempo / per_quarter / 1e6;
        at = change;
        tempo = value;
    }

    // The final segment runs from the last tempo change to `tick`.
    seconds += double(tick - at) * tempo / per_quarter / 1e6;

    return seconds * kTempoScale;
}

// Fills in a report's length, loop and tempo.
void Summarize(const Simulation& sim, const SongPlan& plan, uint32_t per_quarter, SongReport& report)
{
    report.seconds = TickSeconds(sim.actions, per_quarter, plan.end);
    if (plan.loops)
    {
        report.loop_start = TickSeconds(sim.actions, per_quarter, plan.loop_start);
        report.loop_end = TickSeconds(sim.actions, per_quarter, plan.loop_end);
    }

    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kTempo && a.value)
        {
            report.bpm = 60000000.0 / (a.value * kTempoScale);
            break;
        }
    }
}

} // namespace
} // namespace agblib::sequence_export::rare
