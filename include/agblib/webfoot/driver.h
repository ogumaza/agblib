// SPDX-License-Identifier: MIT

// Locating Webfoot Technologies' GBA sound driver in a ROM: its tables, its builds and its songs.
//
// The driver plays Impulse Tracker modules in a packed form: a song names a list of orders and a table of patterns.
// A pattern starts with its count of rows. Each row lists the entries of the channels that change, and ends with a 0
// byte. An entry's first byte gives its channel in the low 4 bits and a mode in the high 4 bits: mode 0 is followed by
// a mask byte, mode 1 takes the channel's last mask, and modes 2 to 15 take a mask from a table of 14. The mask's low 4
// bits say which of the note, instrument, volume and effect bytes follow. Its high 4 bits say which of them repeat the
// channel's last ones. The music plays on 15 channels of a software mixer.
//
// A cartridge can contain several programs, each with a copy of the driver and a song table of its own. Detection finds
// each copy's tables and each song table, and gives a song table the copy of the program that it belongs to.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"

namespace agblib::webfoot
{

using agblib::agb::Rom;

// The driver's music channels, and the effects of its table: Impulse Tracker's letters A to Z are 1 to 26.
constexpr int kChannels = 15;
constexpr int kEffects = 32;

// The notes of the table of steps, and the note that stops a channel's sample.
constexpr int kNotes = 120;
constexpr uint8_t kNoteOff = 255;

// The note whose step is kUnitStep: Impulse Tracker's C-5. A sample's rate is its rate at that note.
constexpr int kMiddleNote = 60;

// The mixer's place in a sample and its step, in 16384ths of a point.
constexpr int kFractionBits = 14;
constexpr uint32_t kUnitStep = 1u << kFractionBits;

// The bits of an entry's mask.
constexpr uint8_t kMaskNote = 0x01;
constexpr uint8_t kMaskInstrument = 0x02;
constexpr uint8_t kMaskVolume = 0x04;
constexpr uint8_t kMaskEffect = 0x08;
constexpr uint8_t kMaskLastNote = 0x10;
constexpr uint8_t kMaskLastInstrument = 0x20;
constexpr uint8_t kMaskLastVolume = 0x40;
constexpr uint8_t kMaskLastEffect = 0x80;

// The modes of an entry that take their mask from the table: 2 to 15.
constexpr int kTableMasks = 14;

// A sample's entry in a sample table: 12 bytes.
constexpr uint32_t kSamplePoints = 0;    // a word: the address of the first point, signed 8-bit
constexpr uint32_t kSampleFlags = 4;     // a halfword: kSampleLoops and kSamplePingPong
constexpr uint32_t kSampleLoopStart = 6; // a halfword: the loop's first point
constexpr uint32_t kSampleLength = 8;    // a halfword: the points, and the loop's end
constexpr uint32_t kSampleRate = 10;     // a halfword: the rate in hertz at kMiddleNote
constexpr uint32_t kSampleSize = 12;

constexpr uint16_t kSampleLoops = 0x01;
constexpr uint16_t kSamplePingPong = 0x02;

// The builds of the driver. They read the same patterns, and differ in the song's header, the mixer's rate and the
// width of a channel's levels.
enum class Build
{
    // The build of Dragon Ball Z: The Legacy of Goku (2002). A header of 12 bytes. The game has one sample table and
    // one table of masks for all its songs. The mixer runs at about 14,000 Hz, and keeps a channel's levels in bytes.
    k2002,
    // The build of the later games (2003 on). A header of 20 bytes, with the song's sample table and table of masks.
    // The mixer runs at about 16,000 Hz, and keeps a channel's levels in halfwords.
    k2003,
};

// The offsets of a song's header. The patterns and the orders come first in both builds.
constexpr uint32_t kHeaderPatterns = 0; // a word: the address of a table of the patterns' addresses
constexpr uint32_t kHeaderOrders = 4;   // a word: the address of the orders, a byte each, ended by kOrderEnd
constexpr uint32_t kHeader2002Tempo = 8;
constexpr uint32_t kHeader2002Speed = 9;
constexpr uint32_t kHeader2002Size = 12;
constexpr uint32_t kHeader2003Samples = 8; // a word: the address of the sample table
constexpr uint32_t kHeader2003Masks = 12;  // a word: the address of the table of masks
constexpr uint32_t kHeader2003Tempo = 16;
constexpr uint32_t kHeader2003Speed = 17;
constexpr uint32_t kHeader2003Size = 20;

// The order that ends the list of orders. The player then goes back to the first order.
constexpr uint8_t kOrderEnd = 0xFF;

// The most orders that a song can have in detection's checks, and the most entries in a row.
constexpr int kMaxOrders = 256;
constexpr int kMaxRowEntries = 64;

// The most bytes between a routine's start and the place where detection reads its code.
inline constexpr uint32_t kRoutineReach = 0x200;

// The first five instructions of the games' master interrupt handler, in ARM: `mov r3, #0x04000000;
// ldr r2, [r3, #0x200]!; ldrh r1, [r3, #8]; mrs r0, spsr; push {r0-r5, lr}` (0x08000138 in The Legacy of Goku,
// 0x03000824 in The Legacy of Goku II).
inline constexpr std::array<uint32_t, 5> kMasterHandlerCode = {0xE3A03640, 0xE5B32200, 0xE1D310B8, 0xE14F0000,
                                                               0xE92D403F};

// The music's volume of the game's options: kStartMusicVolume after the driver's init, up to kFullMusicVolume.
constexpr int kStartMusicVolume = 4;
constexpr int kFullMusicVolume = 8;

// A table of slides: kFineSlides halfwords of fine steps (kFineSlideBytes), then 256 halfwords of slides. The sine of
// the vibratos: kVibratoBytes signed bytes.
constexpr uint32_t kFineSlides = 64;
constexpr uint32_t kFineSlideBytes = 2 * kFineSlides;
constexpr uint32_t kSlideTableBytes = kFineSlideBytes + 2 * 256;
constexpr uint32_t kVibratoBytes = 64;

// The tables of a copy of the driver that the player reads.
struct Tables
{
    uint32_t steps = 0;      // kNotes words: the step of each note at kUnitStep for a sample at the mixer's rate
    uint32_t slide_up = 0;   // the 256 slides up of their table, past its fine steps: Impulse Tracker's linear slides
    uint32_t slide_down = 0; // the 256 slides down of their table, past its fine steps
    uint32_t vibrato = 0;    // the sine of the vibratos
};

// The mixer of a build. The timer's period sets the rate at which the FIFOs play. The step factor sets the rate at
// which a sample's points play: the driver works the steps out from a nominal rate a little off the timer's.
struct Mixer
{
    uint32_t timer_cycles = 0; // the CPU's cycles of a sample
    uint32_t step_factor = 0;  // 2^26 over the nominal rate, rounded up
    uint32_t tick_scale = 0;   // a tick lasts tick_scale / tempo samples
    uint32_t block = 0;        // the most samples that the mix routine mixes at a time
    bool byte_levels = false;  // the levels of a channel are bytes, and a sum of a side wraps in 16 bits

    // Returns the rate in hertz at which the FIFOs play.
    double Rate() const;
};

// Returns the mixer of `build`.
Mixer MixerOf(Build build);

// What detection read of the mixer's settings in a program's init, against MixerOf() for the program's build.
enum class MixerCheck
{
    kSame,    // the init sets the mixer up as MixerOf() has it
    kDiffers, // the init sets another rate or another timer
    kUnread,  // detection found no init, or couldn't read its rate or its timer's reload
};

// A song's header.
struct SongHeader
{
    uint32_t address = 0;
    uint32_t patterns = 0;
    uint32_t orders = 0;
    uint32_t samples = 0; // the sample table
    uint32_t masks = 0;   // the table of masks
    uint8_t tempo = 0;
    uint8_t speed = 0;
};

// A sample's entry.
struct Sample
{
    uint32_t points = 0;
    uint16_t flags = 0;
    uint16_t loop_start = 0;
    uint16_t length = 0;
    uint16_t rate = 0;
};

// A song table, with the copy of the driver that plays its songs.
struct SongTable
{
    uint32_t address = 0;
    Build build = Build::k2003;
    uint32_t program =
        0; // the start of the program in the cartridge that contains the table and the copy of the driver
    Tables tables;
    int music_volume = kStartMusicVolume; // the music's volume that the game starts the songs at
    uint32_t mix_samples = 0;             // the samples of each run of the mix routine; the handler starts the runs
    MixerCheck mixer_check = MixerCheck::kSame;
    std::string mixer_warning; // detection's warning for a mixer_check other than kSame, also in DriverInfo::warnings
    size_t mixer_warning_index = 0; // the place of mixer_warning in DriverInfo::warnings; 0 for kSame
    std::vector<SongHeader> songs;
};

// What detection found. A song's number counts through the song tables in address order.
struct DriverInfo
{
    std::vector<SongTable> song_tables;
    std::vector<std::string> log;
    std::vector<std::string> warnings;

    // Returns the count of songs in all the tables.
    int SongCount() const;

    // Returns the table of song `song`, and sets `index` to the song's place in it. Returns null for a song that no
    // table has.
    const SongTable* TableOf(int song, int& index) const;
};

// Finds the driver in `rom`. Returns false and leaves `error` empty if the game shows no sign of the driver. Returns
// false and sets `error` if the driver's tables are there but its songs can't be found. Warns for a program whose init
// sets the mixer up otherwise than MixerOf() has it for its build, where the model plays its songs at another pitch
// and tempo. Warns too where detection can't read the init's settings. Each song table gives the result of this check
// (SongTable::mixer_check).
bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error);

// Returns the setter of the music volume of the copy of the driver in the program at `program`, found by its code: the
// first at or after the program's start, or 0.
uint32_t FindMusicVolume(const Rom& rom, Build build, uint32_t program);

// Returns the init of the copy of the driver in the program at `program`, found by its code, or 0. The later build's
// init is the routine that passes the music volume of 4 to the setter of FindMusicVolume().
uint32_t FindInit(const Rom& rom, Build build, uint32_t program);

// The 2002 build: returns the mixer's nominal rate that the game passes to the init at `init` from a literal
// (`ldr r0, =rate; bl init`), or 0.
uint32_t FindInitRate(const Rom& rom, uint32_t init);

// Reads the header at `address`. Returns false unless every pattern that the song's orders name reads to its last row
// with entries for channels 1 to 15. In the 2002 build, the sample table and the table of masks are the game-wide ones
// in `samples` and `masks`.
bool ReadSongHeader(const Rom& rom, Build build, uint32_t address, uint32_t samples, uint32_t masks,
                    SongHeader& header);

// Reads the words of the crt0 of the program at `program` that give its image of IWRAM: the image's address in the
// ROM and its size. Returns false if the program's crt0 copies no image to IWRAM.
bool ReadIwramImage(const Rom& rom, uint32_t program, uint32_t& source, uint32_t& size);

// Reads the sample entry at `address`.
Sample ReadSample(const Rom& rom, uint32_t address);

// Returns the count of a song's orders before kOrderEnd, up to kMaxOrders.
int OrderCount(const Rom& rom, const SongHeader& header);

} // namespace agblib::webfoot
