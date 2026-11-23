// SPDX-License-Identifier: MIT

// Frontline Studios' MOD player for the GBA: the older of the two players of the F U H Sound System. It plays modules
// of 4 channels on a software mixer of 8 channels in IWRAM. Once a frame, the game's vertical blank restarts the sound
// DMA on the buffer that the mixer filled in the last frame, runs the player (a tick, or a row at the speed), then
// runs the mixer. The mixer writes 304 samples a frame, mono, for FIFO A.
//
// A module's header is six words: the channels, the count of orders, the count of patterns, the speed, the address of
// the instrument table and the address of the order table. Each order is the address of a pattern's record: its count
// of rows and the address of its cells. An instrument is a loop's start, its end or 0, a volume and the address of a
// sample: its length and the address of its points, signed 8-bit.
//
// A row has a cell for each channel. A cell's first byte contains its flags, and 0 is an empty cell. Bit 0: a period
// index (a halfword) and an instrument (a byte, 0 for the last one). Bit 7: a volume byte. Bits 1 to 6: an effect, as a
// byte offset into the player's table of handlers, with a parameter byte.
//
// The player and the mixer are the same code in every game, apart from the addresses. Detection finds them by their
// code and reads the addresses from their literal pools. The game's code that drives them differs. A table of the games
// gives it, with each game's songs: the orders at which its code starts its modules.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"

namespace agblib::frontline_mod
{

using agblib::agb::Rom;

// A module's header: six words.
constexpr uint32_t kHeaderChannels = 0x00;
constexpr uint32_t kHeaderOrderCount = 0x04;
constexpr uint32_t kHeaderPatternCount = 0x08;
constexpr uint32_t kHeaderSpeed = 0x0C;
constexpr uint32_t kHeaderInstruments = 0x10;
constexpr uint32_t kHeaderOrders = 0x14;
constexpr uint32_t kHeaderSize = 0x18;

// A pattern's record: the count of rows and the address of the cells.
constexpr uint32_t kPatternRows = 0x00;
constexpr uint32_t kPatternCells = 0x04;

// An instrument: the loop's start and end in points from the sample's first point, the end 0 for no loop, the volume
// (0 to 64) and the address of the sample.
constexpr uint32_t kInstrumentLoopStart = 0x00;
constexpr uint32_t kInstrumentLoopEnd = 0x04;
constexpr uint32_t kInstrumentVolume = 0x08;
constexpr uint32_t kInstrumentSample = 0x0C;

// A sample: its length in points and the address of its points.
constexpr uint32_t kSampleLength = 0x00;
constexpr uint32_t kSamplePoints = 0x04;

// The bits of a cell's flags.
constexpr uint8_t kCellNote = 0x01;
constexpr uint8_t kCellEffect = 0x7E;
constexpr uint8_t kCellVolume = 0x80;

// The flags of a cell whose note is the target of a slide: the period index and an instrument byte that the player
// skips.
constexpr uint8_t kCellSlideTarget = 0x11;

// The effects, by their place in the player's table (the cell's byte offset over 4). The entries that the list leaves
// out do nothing.
enum Effect : int
{
    kEffectParameter = 1,      // keeps the parameter
    kEffectSlideDown = 2,      // the period index down by 4 x the parameter a tick
    kEffectSlideUp = 3,        // up by 4 x the parameter a tick
    kEffectSlideToNote = 4,    // toward the target by 4 x the parameter a tick
    kEffectVolume = 5,         // sets the volume
    kEffectVolumeSlide = 6,    // x up, y down a tick, 0 to 64
    kEffectVibrato = 7,        // speed x, depth y
    kEffectVibrato2 = 8,       // the same handler as 7
    kEffectSpeed = 9,          // sets the speed
    kEffectVolumeVibrato = 12, // the volume slide, with the vibrato of the last vibrato's parameter
    kEffectJump = 14,          // to an order at row 0; the rest of the row still plays
    kEffectSampleOffset = 15,  // 256 x the parameter, or the last offset for 0
};

// The effect table's entries. Entry 31 contains no handler.
constexpr int kEffects = 32;

// The songs of a module that a game starts at an order.
struct Module
{
    uint32_t address = 0;
    uint32_t channels = 0;
    uint32_t order_count = 0;
    uint32_t pattern_count = 0;
    uint32_t speed = 0;
    uint32_t instruments = 0;
    uint32_t orders = 0;
};

// The fade step that Hardcore Pool and Garfield pass with each start: the music's volume is full from the song's first
// row.
constexpr int32_t kFullVolume = 256;

// A song: the module, the order at which the game starts it and the fade step that the start passes. A start sets the
// step only while the music's volume is 0. The volume is 0 after the driver's init. It then rises by the step at each
// row. A
// song that no call or table of the game starts is a part of the module that the game never plays. It starts at the
// first order after the jump that ends the song before it, and `unanchored` is set.
struct Song
{
    int module = 0; // its place in DriverInfo::modules
    uint32_t order = 0;
    int32_t step = kFullVolume;
    bool unanchored = false;
};

// A routine of the game's code that the game calls to set up the driver, with its arguments.
struct GameCall
{
    uint32_t routine = 0; // Thumb with bit 0 set
    std::vector<uint32_t> arguments;
};

// The mixer's settings that the game's init passes.
struct MixerSettings
{
    uint32_t channels = 8;    // the mixer's channels: the module's, then the sound effects'
    uint32_t samples = 304;   // a frame's samples
    uint32_t master = 0x50;   // the master volume: a channel's level x master / 64 scales its points, over 64
    uint32_t clip_scale = 64; // the parameter of the builder of the clip table

    // Returns the span of the clip table: the sums from which the table's 256 bytes come, centred on the bias.
    uint32_t ClipSpan() const;
};

// What a frame of the mixer plays at: the GBA's cycles of a frame over the timer's period.
constexpr uint32_t kFrameCycles = 280896;

// The driver in a ROM: where the game copies it to IWRAM, the addresses that its code uses, and the game's set-up.
struct DriverInfo
{
    // The image of IWRAM that the game's start-up code copies: the ROM's byte for 0x03000000, and the part of IWRAM
    // that the copy covers.
    uint32_t image_base = 0;
    uint32_t copy_start = 0;
    uint32_t copy_end = 0;

    // The player's and the mixer's routines in IWRAM (ARM code).
    uint32_t player = 0;
    uint32_t mixer = 0;

    // The player's state, the mixer's context and the tables that the code reads.
    uint32_t state = 0;
    uint32_t context = 0;
    uint32_t effects = 0;     // IWRAM: kEffects words, the handlers
    uint32_t periods = 0;     // ROM: the 16.16 step of each period index
    uint32_t vibrato = 0;     // IWRAM: 64 signed bytes
    uint32_t clip_table = 0;  // IWRAM: 2048 bytes, built by the game's init
    uint32_t output = 0;      // IWRAM: the two buffers of 0x200 bytes that the mixer fills in turn
    uint32_t accumulator = 0; // IWRAM: the mixer's sums

    // The release whose entry in the table of games gives the game's set-up: the image's game code and version, or
    // those of the release whose image the image equals outside the cartridge's header.
    std::string game_code;
    uint8_t game_version = 0;

    // The game's code: its set-up of the driver, the start of a module at an order, and the timer's period.
    std::vector<GameCall> init;
    uint32_t start = 0; // Thumb: r0 = the module's header, r1 = the order, r2 = the fade's step
    uint32_t timer_cycles = 924;
    MixerSettings mixer_settings;

    std::vector<Module> modules;
    std::vector<Song> songs; // module by module: the game's starts, then the parts that no start reaches
    std::vector<std::string> log;
    std::vector<std::string> warnings;

    // Returns the address in the ROM of the IWRAM image's byte for `address`, or 0 outside the image.
    uint32_t ImageAddress(uint32_t address) const;

    // Returns the mixer's rate in hertz.
    double Rate() const;
};

// Finds the driver in `rom`. Returns false and leaves `error` empty if the game shows no sign of the driver. Returns
// false and sets `error` if the driver is there but what the game does with it can't be found.
bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error);

// Reads the header at `address`. Returns false unless every order names a pattern whose cells read to the last row.
bool ReadModule(const Rom& rom, uint32_t address, Module& module);

// Returns the order of the jump that ends the part of `module` that a song starting at `order` plays, and sets
// `target` to the jump's order. Returns -1 if no jump ends it.
int FindJump(const Rom& rom, const Module& module, uint32_t order, uint32_t& target);

} // namespace agblib::frontline_mod
