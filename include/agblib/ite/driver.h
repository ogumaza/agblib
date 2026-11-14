// SPDX-License-Identifier: MIT

// ITE Media's GBA sound driver, the one of Hugo: The Evil Mirror: a player of 4-channel modules with ProTracker's
// command numbers, kept in a bank of songs and samples. This module finds the driver and its bank in a ROM, reads the
// songs, and walks a song's rows as the driver plays them, for the song's loop or end.
//
// The driver mixes into a ring in IWRAM that a FIFO DMA plays at 16384 Hz. It mixes 1920 samples every 7 frames:
// 272 a frame, and 16 more in one frame of the 7. A song's tick lasts 0xA200 / tempo samples of that mix.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"

namespace agblib::ite
{

using agblib::agb::Rom;

// The driver mixes kRingSamples samples in kRingFrames frames.
constexpr int kRingSamples = 1920;
constexpr int kRingFrames = 7;

// A tick lasts kTickSamples / tempo samples, rounded down.
constexpr uint32_t kTickSamples = 0xA200;

// The bank's header: the count of songs and of samples (halfwords), the offset of the sample table and the offsets of
// the songs' headers (words). Every offset counts from the bank's start.
constexpr uint32_t kBankSongs = 0x00;
constexpr uint32_t kBankSamples = 0x02;
constexpr uint32_t kBankSampleTable = 0x08;
constexpr uint32_t kBankSongTable = 0x10;

// A song's header: six halfwords, then the offsets of the patterns' tracks, the order list and the patterns' rows.
constexpr uint32_t kSongHeaderSize = 12;

// The driver's limit of channels.
constexpr int kMaxChannels = 16;

// The flags of an event in a track. Each set flag adds a byte, in this order.
constexpr uint8_t kEventNote = 0x01;
constexpr uint8_t kEventInstrument = 0x02;
constexpr uint8_t kEventCommand = 0x04;
constexpr uint8_t kEventParameter = 0x08;
constexpr uint8_t kEventVolume = 0x10;

// The commands that the walk follows, by ProTracker's numbers.
constexpr uint8_t kCommandPortamento = 0x3;
constexpr uint8_t kCommandOffset = 0x9;
constexpr uint8_t kCommandJump = 0xB;
constexpr uint8_t kCommandVolume = 0xC;
constexpr uint8_t kCommandBreak = 0xD;
constexpr uint8_t kCommandExtended = 0xE;
constexpr uint8_t kCommandSpeed = 0xF;

// A parameter of command F above this sets the tempo. Otherwise it sets the speed, and a speed of 0 ends the song.
constexpr uint8_t kMaxSpeed = 0x1F;

// The driver in a ROM: its init and the bank that the game passes it.
struct DriverInfo
{
    uint32_t init = 0;      // the driver's init (Thumb): its arguments are a mode and the bank
    uint32_t init_call = 0; // the game's call of the init
    uint32_t bank = 0;
    int song_count = 0;
    int sample_count = 0;
    std::vector<std::string> log;
};

// A song's header and the addresses of its tables.
struct Song
{
    uint32_t header = 0;
    int orders = 0;
    int restart = 0;
    int channels = 0;
    int speed = 0;
    int tempo = 0;
    int patterns = 0;
    uint32_t tracks = 0;     // a word for each pattern and channel: the track's offset in the bank
    uint32_t order_list = 0; // a byte for each order: its pattern
    uint32_t rows = 0;       // a byte for each pattern: its rows
};

// Finds the driver's init by the literal pool of its two copies of ARM code to IWRAM, the game's call of it and the
// bank that the call passes. Returns false and sets `error` when the ROM has the pool but no init before it, or no call
// of the init that passes a bank that can be read. Returns false with `error` empty when the ROM has no such pool.
bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error);

// Reads song `index` of the bank at `bank`. Returns false for a number past the bank's songs or a song whose tables
// fall outside the ROM. A song without patterns reads, and the driver can't play it.
bool ReadSong(const Rom& rom, uint32_t bank, int index, Song& song);

// How a song's rows go: a loop, an end on command F00, or neither within the walk's limit. Times are samples of the
// driver's mix from the song's start, where the driver reads its first row.
struct Flow
{
    enum class Kind
    {
        kLoop,
        kEnd,
        kUnknown,
    };

    Kind kind = Kind::kUnknown;
    uint64_t end = 0;        // kEnd: the tick after the F00; kUnknown: where the walk stopped
    uint64_t loop_start = 0; // kLoop: the latest earlier entry of the order that the loop goes back to
    uint64_t loop_end = 0;   // kLoop: the entry that goes back to it
    int loop_order = -1;
    int notes = 0; // the note events that the walk read
};

// Walks song `song` of the bank at `bank` as the driver's tick and row routines play it: the order list, the restart
// order, commands B, D and F and the parameter byte that a command without one reuses. A loop is the first entry of an
// order that comes after an entry of the same or a later order, where the song entered that order before. The walk
// stops there, so an F00 after it never counts. The loop's start is the order's entry, not a repeat of the channels'
// state: a command or a note left from the last pattern can play on into the next pass, and the state can first repeat
// an order or two later. The walk also stops after `max_samples`. A song without patterns or with a tempo that makes a
// tick 0 samples long gives kUnknown at once.
Flow WalkSong(const Rom& rom, uint32_t bank, const Song& song, uint64_t max_samples);

// The frames and the seconds that `samples` samples of the driver's mix take.
double FramesOfSamples(uint64_t samples);
double SecondsOfSamples(uint64_t samples);

} // namespace agblib::ite
