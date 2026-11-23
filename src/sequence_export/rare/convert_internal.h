// SPDX-License-Identifier: MIT

// Definitions that the parts of the Rare conversion share (convert.cpp, convert_simulation.h and convert_notes.h): the
// names they use, and the simulation of a tune and its notes. Only those files include this header. The definitions
// therefore keep internal linkage.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/files.h"
#include "agblib/agb/rom.h"
#include "agblib/rare/driver.h"
#include "agblib/rare/sequencer.h"
#include "agblib/rare/song.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/rare/convert.h"
#include "agblib/sequence_export/rare/soundfont.h"

namespace agblib::sequence_export::rare
{

using agblib::agb::HexBytes;
using agblib::agb::LoopStart;
using agblib::agb::PlanSong;
using agblib::agb::SongPlan;
using agblib::agb::TickSeconds;
using agblib::agb::TrackLayout;
using agblib::agb::WriteTextFile;
using agblib::rare::Action;
using agblib::rare::DecodeEvent;
using agblib::rare::DescribeEvent;
using agblib::rare::Event;
using agblib::rare::IsDelay;
using agblib::rare::kChannels;
using agblib::rare::kCmdControl;
using agblib::rare::kCmdEnd;
using agblib::rare::kCtrlLoopEnd;
using agblib::rare::kCtrlLoopStart;
using agblib::rare::kFrameTime;
using agblib::rare::kSemitone;
using agblib::rare::kTempoScale;
using agblib::rare::MixResult;
using agblib::rare::MusicVolume;
using agblib::rare::ReadInstrument;
using agblib::rare::ReadTuneHeader;
using agblib::rare::ScanTrack;
using agblib::rare::Sequencer;
using agblib::rare::SilencedChannels;
using agblib::rare::Slot;
using agblib::rare::TuneAddress;
using agblib::rare::TuneHeader;
using agblib::sequence_export::agb::FixedBendValue;
using agblib::sequence_export::agb::kDrumChannel;
using agblib::sequence_export::agb::MidiFile;
using agblib::sequence_export::agb::MidiTrack;
using agblib::sequence_export::agb::PresetInstrument;
using agblib::sequence_export::agb::SongNumber;
using agblib::sequence_export::agb::SongStem;
using agblib::sequence_export::agb::SquareRootValue;

namespace cc = seqlib::midi::cc;

namespace
{

// A channel pitch-offset change and its tick.
struct PitchChange
{
    uint64_t tick;
    int channel;
    int64_t offset;
    uint32_t frame;
};

// A change in whether the mixer leaves out every voice of a key on a channel, at the tick of the frame it happens in.
struct MuteChange
{
    uint64_t tick;
    int channel;
    int key;
    bool muted;
    uint32_t frame;
};

// Results of running a tune through the model: track actions, channel pitch changes, mute changes and warnings.
struct Simulation
{
    std::vector<Action> actions;
    std::vector<PitchChange> pitches;
    std::vector<MuteChange> mutes;
    std::vector<uint64_t> frame_starts; // with frame timing, the tick each frame starts at
    std::vector<std::string> warnings;
    std::optional<uint64_t> cut_off;                       // the tick where the model gave up, at the frame limit
    std::array<EnvelopeSettings, kChannels> end_envelopes; // each channel's settings from controllers 20-23 at the end
    std::vector<agblib::rare::TrackStretch> stretches;     // with `stretches`, the tracks' stretches of commands
};

// A note as the MIDI file plays it.
struct Note
{
    int track = 0;
    int channel = 0;
    int key = 0;
    int velocity = 0;
    int level = 0; // the velocity + 1 of each of the driver's voices that play it, added up
    int program = -1;
    int bank = 0; // the bank of its preset
    uint32_t instrument = 0;
    EnvelopeSettings envelope; // the settings from controllers 20-23 that its envelope's phases took
    uint64_t on = 0;
    uint64_t off = 0;
    uint32_t frame = 0;   // the frame the driver starts it on
    uint32_t command = 0; // the address of the note on that started it
    bool open = true;
    int voices = 1;               // the driver's voices that play it
    bool before_programs = false; // it plays with the program from before its track's program changes on its tick
};

// Works out the MIDI file's notes from the model's note ons and offs. A MIDI channel can't play one key twice at once.
// Where the driver's channel does, the older note ends when the newer one starts, unless they start on the same frame.
// A note ends where the driver releases its slot, where a later note takes its slot, or at the end of the conversion.
class NoteCollector
{
public:
    // Collects the notes of the tracks in `track_mask` from `sim`. `slot_count` is the number of the model's note
    // slots. `sim` and `plan` have to outlive the collector.
    NoteCollector(const Simulation& sim, const SongPlan& plan, uint32_t track_mask, int slot_count);

    // Returns the notes in the order of the model's note ons. Call it once.
    std::vector<Note> Collect();

private:
    // Starts the MIDI note of a note on. A note on of a key that started a note on the same frame adds a voice to that
    // note instead.
    void NoteOn(const Action& a);

    // Ends the note that a note off releases.
    void NoteOff(const Action& a);

    // Gives the note that a slot played last the settings that a phase of the slot's envelope took.
    void TakeSettings(const Action& a);

    // Returns the action that gives a note on its program: the note on itself for a note that the model plays. A note
    // before its channel's first program change takes the change at its tick, if there's one. Returns null for any
    // other note that the model doesn't play.
    const Action* ProgramFor(const Action& a) const;

    // Ends note `index` at `tick`, unless it has ended already.
    void Close(int index, uint64_t tick);

    // Ends a slot's voice, and the note when its last voice ends.
    void Release(int slot, uint64_t tick, uint32_t frame);

    const Simulation& sim_;
    const SongPlan& plan_;
    uint32_t track_mask_;
    std::vector<Note> notes_;
    std::array<std::array<int, 128>, kChannels> owner_;  // each key's playing note, or -1
    std::array<std::array<int, 128>, kChannels> latest_; // each key's last note, ended or not, or -1
    std::vector<int> slot_note_;                         // the note of each slot's voice, or -1
    std::vector<int> slot_level_;                        // the velocity + 1 of each slot's voice

    // The note that each slot played last, or -1. The release of the slot's voice is part of the note.
    std::vector<int> slot_last_;

    // The first program change on each channel at each tick.
    std::map<std::pair<int, uint64_t>, const Action*> first_program_;
};

} // namespace
} // namespace agblib::sequence_export::rare
