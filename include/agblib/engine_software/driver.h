// SPDX-License-Identifier: MIT

// Locating Engine Software's GBA sound driver in a ROM: its routines, its build, its song bank and the game's set-up.
//
// The driver plays modules in the manner of FastTracker 2 on a software mixer. The driver is Thumb code. Its mixer is
// ARM code that the driver copies to IWRAM. The mixer writes 8-bit samples for FIFO A, mono or stereo, at one of three
// rates. A game calls the driver's frame routine once a frame. The routine runs a tick whenever the samples left to
// the next tick reach 0, and mixes the frame's samples in pieces between the ticks. Ticks therefore fall inside a
// frame, at a sample.
//
// A game's music is a bank: a 4-byte header, a word for each song, the instruments with their samples inline, a pool
// of packed rows, then the songs. A song is a header, an order list and its patterns. A pattern is a list of words, one
// for each row: the row's offset in the bank, or 0 for an empty row. A packed row starts with a bit for each of the 5
// fields of each channel (the note, the instrument, the volume column, the effect and its operand), the first channel
// in the high bit of the first byte. A byte follows for each set bit.
//
// The driver's code differs from game to game in its limits, its effects and its interrupts. A table of builds gives
// each game's facts. Detection finds the mixer's channel handler by its code, names the build from the game's code and
// checks the build's routines by their first instructions. It reads the bank's address from the game's call of the
// bank's load.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"

namespace agblib::engine_software
{

using agblib::agb::Rom;

// The bank's header.
constexpr uint32_t kBankKind = 0;        // 0x21: not read by the driver
constexpr uint32_t kBankVersion = 1;     // 1
constexpr uint32_t kBankInstruments = 2; // the count of instruments
constexpr uint32_t kBankSongs = 3;       // the count of songs
constexpr uint32_t kBankSongTable = 4;   // a word for each song: the song's offset from the bank
constexpr uint8_t kBankVersionOne = 1;

// An instrument's record. Its samples follow it, signed 8-bit. The next record starts at the record plus
// (length + kInstrumentSamples + 3) & ~3.
constexpr uint32_t kInstrumentLength = 0x00;       // a word: the samples in bytes
constexpr uint32_t kInstrumentLoopStart = 0x04;    // a word
constexpr uint32_t kInstrumentLoopBytes = 0x08;    // a word: 0 for a sample that plays once
constexpr uint32_t kInstrumentVolume = 0x0C;       // 0 to 64
constexpr uint32_t kInstrumentPan = 0x0D;          // 0 to 255
constexpr uint32_t kInstrumentFineTune = 0x0E;     // signed: 128ths of a semitone
constexpr uint32_t kInstrumentRelativeNote = 0x0F; // signed
constexpr uint32_t kInstrumentFadeOut = 0x10;      // a word: subtracted from the fade-out level each tick
constexpr uint32_t kInstrumentVolumeEnvelope = 0x14;
constexpr uint32_t kInstrumentPanEnvelope = 0x48;
constexpr uint32_t kInstrumentSamples = 0x7C;

// An envelope: its count of points, its sustain point, its loop's start and end points (0xFF for none), then 12
// points of a halfword tick and a halfword value.
constexpr uint32_t kEnvelopeCount = 0;
constexpr uint32_t kEnvelopeSustain = 1;
constexpr uint32_t kEnvelopeLoopStart = 2;
constexpr uint32_t kEnvelopeLoopEnd = 3;
constexpr uint32_t kEnvelopePoints = 4;
constexpr uint32_t kEnvelopePointSize = 4;
constexpr uint8_t kEnvelopeNone = 0xFF;

// A song's header, then its order list from kSongOrderList, padded to a word, then its patterns.
constexpr uint32_t kSongChannels = 0;
constexpr uint32_t kSongOrders = 1;
constexpr uint32_t kSongRestart = 2;
constexpr uint32_t kSongPatterns = 3;
constexpr uint32_t kSongSpeed = 4;
constexpr uint32_t kSongTempo = 5;
constexpr uint32_t kSongOrderList = 8;

// A pattern: a halfword count of rows, a halfword 0, then a word for each row.
constexpr uint32_t kPatternRows = 4;

// The fields of a channel in a packed row, and the note that releases a channel.
constexpr int kRowFields = 5;
constexpr uint8_t kNoteKeyOff = 0x61;

// The effects, by their number in a row: FastTracker 2's, with G as 0x10 and so on. The others call the game's
// callback.
enum Effect : uint16_t
{
    kEffectArpeggio = 0x00,
    kEffectSlideUp = 0x01,
    kEffectSlideDown = 0x02,
    kEffectSlideToNote = 0x03,
    kEffectVibrato = 0x04,
    kEffectSlideToNoteVolume = 0x05,
    kEffectVibratoVolume = 0x06,
    kEffectTremolo = 0x07,
    kEffectPan = 0x08,
    kEffectOffset = 0x09,
    kEffectVolumeSlide = 0x0A,
    kEffectJump = 0x0B,
    kEffectVolume = 0x0C,
    kEffectBreak = 0x0D,
    kEffectExtended = 0x0E,
    kEffectSpeed = 0x0F,
    kEffectMusicVolume = 0x10,
    kEffectMusicVolumeSlide = 0x11,
    kEffectKeyOff = 0x14,
    kEffectEnvelopePlace = 0x15,
    kEffectRetrigger = 0x1B,
};

// The commands of effect 0x0E, by the operand's high 4 bits.
constexpr uint8_t kExtendedFineTune = 0x5;
constexpr uint8_t kExtendedFineVolumeUp = 0xA;
constexpr uint8_t kExtendedFineVolumeDown = 0xB;
constexpr uint8_t kExtendedNoteDelay = 0xD;
constexpr uint8_t kExtendedPatternDelay = 0xE;

// The pitch: a note's index is (note + relative note) x 128 + fine tune + slide. The driver's pitch table gives the
// sample rate in hertz at each index: halfwords up to kPitchHalfwords, words after. Index kMiddleIndex, note 49, plays
// 8,363 Hz. kPitchWords words follow the halfwords, to index 15,231.
constexpr int kNoteSteps = 128;
constexpr uint32_t kPitchHalfwords = 0x2A53;
constexpr uint32_t kPitchWords = 4397;
constexpr int kMiddleNote = 49;
constexpr int kMiddleIndex = kMiddleNote * kNoteSteps;

// The points of the vibrato's and the tremolo's wave.
constexpr uint32_t kSinePoints = 64;

// The mixer's place in a sample and its step: 16 bits of fraction.
constexpr int kFractionBits = 16;

// A channel's record in IWRAM, and the most channels of a build: music and effects.
constexpr uint32_t kRecordSize = 0x4C;
constexpr int kMaxRecords = 10;

// The routines of a build that a stub or a check calls. Addresses of Thumb code have bit 0 clear.
struct Routines
{
    uint32_t reset = 0;        // r0 the game's allocator, r1 its free
    uint32_t init = 0;         // r0 1 for stereo, r1 the rate in hertz, r2 the clip table's gain (0 for the default)
    uint32_t bank_load = 0;    // r0 the bank
    uint32_t effect_set = 0;   // r0 the effect channels, r1 the table of effects
    uint32_t play_number = 0;  // r0 the song's number, with the flags in bits 8 and up (0x100: a jingle)
    uint32_t music_volume = 0; // r0 0 to 256
    uint32_t frame = 0;        // runs the ticks and mixes a frame's samples
    uint32_t swap = 0;         // restarts the sound DMA on the buffer that the mixer filled and swaps the buffers
    uint32_t mixer = 0;        // the image of the mono mixer that the driver copies to IWRAM
    uint32_t code_start = 0;   // the start of the driver's code
    uint32_t code_end = 0;     // the end of the driver's code
};

// The game's sound manager: a C++ object whose pointer points to a block of fields. The game's code reaches the
// driver's music volume only through it. Its start sets the music volume to 0 and starts a song, and its routine of
// the vertical count raises the volume in steps toward its target before it runs the frame routine. The manager's
// set-up allocates the block on the game's heap. A stub or a check therefore places the object and the block in RAM
// and sets their fields as the set-up does.
struct Manager
{
    uint32_t set_music_volume = 0;  // r0 the object, r1 the volume: sets it at once
    uint32_t set_effect_volume = 0; // r0 the object, r1 the volume
    uint32_t start = 0;             // r0 the object, r1 the song, r2 the frames of the ramp, r3 its target
    uint32_t jingle = 0;            // r0 the object, r1 the song: plays it once over the song that plays
    uint32_t vblank = 0;            // r0 the object: the routine that the game's vertical blank calls
    uint32_t vcount = 0;            // r0 the object: the ramps and the frame routine, at the vertical count
    uint32_t music_ramp = 0;        // r0 the object: a step of the music volume's ramp
    uint32_t block_size = 0;        // the bytes of the block of fields
    uint32_t music_volume = 0;      // the music volume of the set-up and of a song's start: the ramp's target
    uint32_t effect_volume = 0;     // the effect volume of the set-up
    uint32_t ramp_frames = 0;       // the frames of the ramp at a song's start
    uint32_t object = 0;            // where a stub or a check places the object, with the block after it
};

// Where a stub or a check places the block of fields: after the object, a word that points to it.
constexpr uint32_t kManagerBlock = 4;

// The fields of the manager's block.
constexpr uint32_t kManagerEffectVolume = 0x00;
constexpr uint32_t kManagerMusicVolume = 0x04;
constexpr uint32_t kManagerMusicStep = 0x08;
constexpr uint32_t kManagerMusicTarget = 0x0C;
constexpr uint32_t kManagerBusy = 0x18; // set by the routine of the vertical count, cleared by the vertical blank's
constexpr uint32_t kManagerFlag = 0x19; // 1 after the set-up

// How the game starts a song: a ramp of the music volume from 0 over `frames` frames, or a jingle that plays once.
// `one_pass` marks a song that the driver loops and the game always ends within its first pass. Its conversion ends
// where the driver would start the second pass.
struct SongStart
{
    bool jingle = false;
    uint32_t frames = 0;
    bool one_pass = false;
};

// The facts of a build of the driver in one game. A game's build is named by its code.
struct Build
{
    const char* code = "";
    const char* name = "";
    bool supported = false; // whether the model follows this build
    uint32_t handler = 0;   // the mono mixer's handler of a looping channel, in the cartridge
    Routines routines;
    Manager manager;
    uint32_t mixer_state = 0; // IWRAM: the mixer's state
    uint32_t channels = 0;    // IWRAM: the channels' records
    uint32_t sequencer = 0;   // IWRAM: the sequencer's state
    uint32_t sine = 0;        // kSinePoints signed halfwords: the vibrato's and the tremolo's wave
    uint32_t pitch = 0;       // the pitch table's halfwords; its words follow at pitch_words
    uint32_t pitch_words = 0;
    uint32_t allocator = 0; // the game's routines that the reset takes (Thumb, bit 0 set)
    uint32_t free = 0;
    bool stereo = false;
    uint32_t rate = 0; // the rate that the game passes to the init
    uint32_t gain = 0; // the clip table's gain that the game passes
    uint32_t effect_channels = 0;
    uint32_t effect_table = 0;
    uint32_t music_channels = 0;   // the most music channels of the build
    uint16_t waitcnt = 0;          // WAITCNT as the game sets it
    uint16_t dispstat = 0;         // DISPSTAT: the vertical count's line in the high byte, and its interrupt
    uint16_t interrupts = 0;       // IE
    std::vector<SongStart> starts; // by song
};

// Returns every build that detection finds, the ones that the model doesn't follow included.
const std::vector<Build>& Builds();

// A song: its header and its tables.
struct Song
{
    uint32_t address = 0;
    uint8_t channels = 0;
    uint8_t orders = 0;
    uint8_t restart = 0;
    uint8_t pattern_count = 0;
    uint8_t speed = 0;
    uint8_t tempo = 0;
    uint32_t order_list = 0;
    std::vector<uint32_t> patterns; // the address of each pattern
    SongStart start;
};

// The bank: its header and its instruments.
struct Bank
{
    uint32_t address = 0;
    uint8_t kind = 0;
    std::vector<uint32_t> instruments; // the address of each instrument's record
    std::vector<uint32_t> song_offsets;
};

// The driver in a ROM.
struct DriverInfo
{
    Build build;
    Bank bank;
    std::vector<Song> songs;
    std::vector<std::string> log;
    std::vector<std::string> warnings;

    // Returns the samples that the mixer writes a frame, the step's factor and the timer's rate in hertz, for the
    // build's rate.
    uint32_t FrameSamples() const;
    uint32_t StepFactor() const;
    double OutputRate() const;
};

// A step of the game's code that sets up the driver or starts a song: a call of a Thumb routine with up to four
// arguments, or a store of `size` bytes (1, 2 or 4) of `values[0]` at `address`.
struct GameStep
{
    uint32_t address = 0;         // the routine, with bit 0 set, or the address of the store
    std::vector<uint32_t> values; // the call's arguments, or the store's value
    int size = 0;                 // 0 for a call
};

// Returns the game's set-up of its sound manager and of the driver, with the manager's object at
// `info.build.manager.object` and its block of fields after it: the manager's set-up without its allocation of the
// block (0x0802CF44 in B33E). The steps clear the block and point the object at it, set the music and effect volumes,
// mark the set-up done, then make the driver's reset, init, bank load and effect set with the game's values.
std::vector<GameStep> SetUpSteps(const DriverInfo& info);

// Returns the game's start of song `song` of `info.songs`: the manager's start with its ramp of the music volume up
// from 0, or for a jingle the manager's music volume, then the manager's jingle. A jingle plays the song once.
std::vector<GameStep> StartSteps(const DriverInfo& info, int song);

// Returns the samples a frame, and the step's factor, for the rate that the init stores, as its code sets them: 96 at
// 5,734 Hz, 176 at 10,512 Hz and 224 at 13,378 Hz. Another rate leaves them as they were: the returns are then 0.
uint32_t FrameSamplesAt(uint32_t rate);
uint32_t StepFactorAt(uint32_t rate);

// Returns the rate that the init stores for `rate`: at most 13,378 Hz.
uint32_t StoredRate(uint32_t rate);

// Returns the mixer's samples a tick at `tempo`: the rate over (tempo x 0x333 x 0x1001) >> 23, a tempo's 2/5. Returns
// 0 where that divisor is 0.
uint32_t TickSamples(uint32_t rate, uint8_t tempo);

// Returns the pitch table's rate in hertz at `index`, a note's index, and the address of its entry: a halfword up to
// kPitchHalfwords, a word after. A negative index reads past the words, as the driver's unsigned compare does.
uint32_t PitchEntry(const Build& build, int32_t index);
uint32_t PitchRate(const Rom& rom, const Build& build, int32_t index);

// Returns the mixer's step at `index`: the rate times the step's factor, over 256, in 16.16 points a sample.
uint32_t PitchStep(const Rom& rom, const DriverInfo& info, int32_t index);

// Finds the driver in `rom`. Returns false and leaves `error` empty if the game shows no sign of the driver. Returns
// false and sets `error` if the driver is there but its build isn't supported, or the bank or a song can't be read.
bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error);

// Reads the bank at `address`: its header, its instruments and its songs, with each pattern. Returns false and sets
// `error` where a record leaves the cartridge or a song's patterns don't fit.
bool ReadBank(const Rom& rom, uint32_t address, Bank& bank, std::vector<Song>& songs, std::string& error);

// Returns the end of the instrument record at `record`: the record and its samples, rounded up to a word.
uint32_t InstrumentEnd(const Rom& rom, uint32_t record);

} // namespace agblib::engine_software
