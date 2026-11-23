// SPDX-License-Identifier: MIT

// Frontline Studios' XM-like player for the GBA: the newer of the two players of the F U H Sound System, the one of
// Chicken Shoot and Chicken Shoot 2. It plays compiled modules of 4 channels, in the format of the company's DS player,
// on a software mixer that writes mono samples for FIFO A.
//
// The player is ARM code in the cartridge. The game calls its mix routine once a frame. The routine mixes the frame's
// samples in segments: whenever the samples left to the next tick reach 0, it runs a tick and mixes on. A tick reads a
// row at the speed. Ticks therefore fall inside a frame, at a sample.
//
// A module's header is 20 bytes: the channels, the patterns, the master volume, the note handler, the last order, the
// restart, the speed and the BPM, then the addresses of the order list, the pattern table and the instrument table.
// A game plays an order list of its own with the module: a song is a module, a list and its last order.
//
// The player's code is the same in both games, at addresses that one offset moves. Detection finds its mix routine by
// its code, checks the other routines at their distances from it, and reads the addresses that the code uses from its
// literal pools. A table of the games gives the game's set-up and the lists that it plays.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"

namespace agblib::frontline_xm
{

using agblib::agb::Rom;

// A module's header.
constexpr uint32_t kHeaderChannels = 0x00;
constexpr uint32_t kHeaderPatternCount = 0x01;
constexpr uint32_t kHeaderMaster = 0x02;
constexpr uint32_t kHeaderNoteHandler = 0x03;
constexpr uint32_t kHeaderLastOrder = 0x04;
constexpr uint32_t kHeaderSpeed = 0x06;
constexpr uint32_t kHeaderBpm = 0x07;
constexpr uint32_t kHeaderOrders = 0x08;
constexpr uint32_t kHeaderPatterns = 0x0C;
constexpr uint32_t kHeaderInstruments = 0x10;

// A pattern: its rows (a halfword), a byte, then the cells.
constexpr uint32_t kPatternCells = 3;

// The bits of a cell's flags.
constexpr uint8_t kCellNote = 0x01;   // a note and an instrument
constexpr uint8_t kCellVolume = 0x10; // a byte of the volume column
constexpr uint8_t kCellEffect = 0x20; // an effect and its parameter

// An instrument: its length, loop start and loop end in 1/8192 points, its volume (0 to 64), its finetune (signed),
// its loop type (0 for none) and its points, signed 8-bit.
constexpr uint32_t kInstrumentLength = 0x08;
constexpr uint32_t kInstrumentLoopStart = 0x0C;
constexpr uint32_t kInstrumentLoopEnd = 0x10;
constexpr uint32_t kInstrumentVolume = 0x14;
constexpr uint32_t kInstrumentFinetune = 0x15;
constexpr uint32_t kInstrumentLoopType = 0x16;
constexpr uint32_t kInstrumentPoints = 0x1F;

// A position counts points in 1/8192.
constexpr int kPositionShift = 13;

// The channels that the mixer plays.
constexpr int kChannels = 4;

// The effects, by their number in a cell: the place of their handler in the player's table.
enum Effect : int
{
    kEffectBpm = 1,               // sets the BPM, and with it the samples a tick
    kEffectSlideUp = 2,           // lowers the period by 4 x the parameter a tick
    kEffectSlideDown = 3,         // raises it by 4 x the parameter a tick
    kEffectSlideToNote = 4,       // toward the cell's note by 4 x the parameter a tick
    kEffectVolume = 5,            // sets the volume
    kEffectVolumeSlide = 6,       // x up, or the parameter down when y isn't 0, a tick
    kEffectVibrato = 7,           // speed x, depth y
    kEffectVolumeColumn = 8,      // the cell's volume byte, run again
    kEffectSpeed = 9,             // sets the speed
    kEffectArpeggio = 10,         // the period x semitones up at the first tick after the row, y at the second
    kEffectSlideAndVolume = 11,   // the tick routine runs the whole tick again: not followed
    kEffectVibratoAndVolume = 12, // the same
    kEffectTremolo = 13,          // speed x, depth y: changes the volume for good
    kEffectJump = 14,             // to an order at the next tick
    kEffectOffset = 15,           // starts the sample 256 x the parameter points in
    kEffectBreak = 16,            // the next order, at a row
    kEffectFineSlideUp = 17,      // lowers the sounding period by 16 x the parameter
    kEffectFineSlideDown = 18,    // raises it by the parameter / 16
    kEffectVibratoWave = 20,      // the sine for a parameter that isn't 0, else the saw
    kEffectTremoloWave = 23,      // the same for the tremolo
    kEffectRetrigger = 24,        // counts ticks, but its routine plays nothing
    kEffectFineVolumeUp = 25,
    kEffectFineVolumeDown = 26,
    kEffectCut = 27,          // the volume to 0 after the parameter's ticks
    kEffectDelay = 28,        // the note after the parameter's ticks
    kEffectPatternDelay = 29, // keeps the parameter, and nothing reads it
};

// The bytes of each of the player's waves: 256 signed bytes.
constexpr uint32_t kWaveSize = 256;

// The player's routines, its tables and its state. The routines are ARM code.
struct Library
{
    uint32_t mix = 0;                   // the frame's mix: ticks and segments
    uint32_t load = 0;                  // r0 the module's header less 4, r1 0
    uint32_t play = 0;                  // r0 an order list, r1 the first order, r2 the last
    uint32_t volume = 0;                // r0 the master volume, 0 to 255
    uint32_t music = 0;                 // r0 1 to switch the music's mix on
    uint32_t output = 0;                // starts the sound DMAs at the next vertical blank
    uint32_t frame = 0;                 // the game's call once a frame: the mixes that are on
    uint32_t swap = 0;                  // the vertical blank's call: the DMAs' restart and the swap of the buffers
    uint32_t state = 0;                 // IWRAM: the player's state
    uint32_t channels = 0;              // IWRAM: the channels' records, 60 bytes each
    uint32_t frame_samples = 0;         // halfwords: the samples a frame, by the rate's index
    uint32_t rates = 0;                 // halfwords: the rate in hertz, by the rate's index
    uint32_t step_rates = 0;            // words: the step's factor, by the rate's index
    uint32_t periods = 0;               // halfwords: a period's place in the table of steps
    uint32_t steps = 0;                 // words
    uint32_t silence = 0;               // the point that a stopped channel plays
    std::array<uint32_t, 3> waves = {}; // kWaveSize bytes each: the sine, the square and the saw
};

// The state's fields that the game and the player read and write.
constexpr uint32_t kStateFrameSamples = 0x00;
constexpr uint32_t kStateOutput = 0x58; // the buffer that the next mix fills

// A module's header and its tables.
struct Module
{
    uint32_t address = 0;
    uint8_t channels = 0;
    uint8_t pattern_count = 0;
    uint8_t master = 0;
    uint8_t note_handler = 0;
    uint8_t last_order = 0;
    uint8_t speed = 0;
    uint8_t bpm = 0;
    uint32_t orders = 0;
    uint32_t patterns = 0;
    uint32_t instruments = 0;
};

// A list of orders that the game plays with a module, at a master volume.
struct Song
{
    int module = 0; // its place in DriverInfo::modules
    uint32_t list = 0;
    uint8_t last = 0;
    uint8_t volume = 0;
};

// The driver in a ROM.
struct DriverInfo
{
    Library library;
    uint32_t sound_init = 0; // the game's set-up of the player (Thumb)
    uint8_t rate_index = 0;  // the mixer's rate, from the set-up
    std::vector<Module> modules;
    std::vector<Song> songs; // the title's, then the levels' in the order of the game's table
    std::vector<std::string> log;
    std::vector<std::string> warnings;

    // Returns the samples that the mixer writes a frame, and its rate in hertz.
    uint32_t FrameSamples(const Rom& rom) const;
    uint32_t Rate(const Rom& rom) const;
};

// Finds the driver in `rom`. Returns false and leaves `error` empty if the game shows no sign of the driver. Returns
// false and sets `error` if the driver is there but the game's set-up or songs can't be found.
bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error);

// Returns the step of `period`, in 1/8192 points a sample: the table of periods gives the period's place in the table
// of steps and a shift, and the rate's factor scales the step.
uint32_t StepOf(const Rom& rom, const DriverInfo& info, uint32_t period);

// Reads the header at `address`. Returns false unless every order of the module's list names a pattern whose cells
// read to its last row.
bool ReadModule(const Rom& rom, uint32_t address, Module& module);

} // namespace agblib::frontline_xm
