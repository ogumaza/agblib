// SPDX-License-Identifier: MIT

// A synthetic cartridge with Webfoot Technologies' GBA driver, for tests: the driver's tables, the code that detection
// reads, and modules of songs and samples. A module can also go into a copy of a game's image, and the game's copy of
// the driver then plays it. The crafted module plays every effect of the driver, its ping-pong and forward loops and
// its one-shot samples.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <string_view>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/webfoot/driver.h"

namespace agblib::testing::webfoot_cart
{

using agblib::agb::kRomBase;
using agblib::testing::agb_files::CartImage;
using namespace agblib::webfoot;

// A sample: `length` points, looped from `loop_start` if `flags` says so. Each point is `level`, or with `saw` set a
// sawtooth from -level to level over 64 points. The sawtooth shows a voice's place in its output.
struct TestSample
{
    int8_t level = 64;
    uint16_t length = 1000;
    uint16_t flags = kSampleLoops;
    uint16_t loop_start = 0;
    uint16_t rate = 16000;
    bool saw = false;
};

// An entry of a row. With `last_mask` set, the entry takes the channel's last mask (mode 1). That mask has to be
// `mask`.
struct Entry
{
    int channel = 0; // from 0
    uint8_t mask = 0;
    uint8_t note = 0;
    uint8_t instrument = 0;
    uint8_t volume = 0;
    uint8_t effect = 0;
    uint8_t parameter = 0;
    bool last_mask = false;
};

using Row = std::vector<Entry>;

// A song: its patterns, played in turn by its orders, and its tempo and speed.
struct TestSong
{
    std::vector<std::vector<Row>> patterns;
    uint8_t tempo = 125;
    uint8_t speed = 6;
};

// The masks of the test's table: mode 2 gives a note and an instrument, and mode 3 a volume and an effect.
constexpr uint8_t kNoteInstrument = kMaskNote | kMaskInstrument;
constexpr uint8_t kVolumeEffect = kMaskVolume | kMaskEffect;

// A note with its instrument and volume, and no effect.
inline Entry NoteEntry(int channel, uint8_t note, uint8_t instrument, uint8_t volume)
{
    return {channel, kMaskNote | kMaskInstrument | kMaskVolume, note, instrument, volume, 0, 0};
}

// A note with its instrument, volume and an effect.
inline Entry NoteEffectEntry(int channel, uint8_t note, uint8_t instrument, uint8_t volume, uint8_t effect,
                             uint8_t parameter)
{
    return {channel,  kMaskNote | kMaskInstrument | kMaskVolume | kMaskEffect, note, instrument, volume, effect,
            parameter};
}

inline Entry EffectEntry(int channel, uint8_t effect, uint8_t parameter)
{
    return {channel, kMaskEffect, 0, 0, 0, effect, parameter};
}

// A pattern of `count` rows whose first row is `first`.
inline std::vector<Row> Pattern(const Row& first, size_t count)
{
    std::vector<Row> rows(count);
    rows[0] = first;

    return rows;
}

// The places of a module's parts, from its base.
constexpr uint32_t kModuleSamples = 0x2000;
constexpr uint32_t kModuleMasks = 0x2100;
constexpr uint32_t kModuleSongs = 0x2200;
constexpr uint32_t kModuleOrders = 0x2400;
constexpr uint32_t kModulePatternTable = 0x2600;
constexpr uint32_t kModulePatterns = 0x3000;
constexpr uint32_t kModulePoints = 0x8000;
constexpr uint32_t kModuleSongLiterals = 0x500;
constexpr uint32_t kModuleSize = 0x10000;

// The most songs of a module.
constexpr uint32_t kModuleMaxSongs = 8;

// Writes a module of songs and samples into an image from `base` on. In the 2002 build, the songs use the sample table
// and the table of masks at `samples` and `masks`. The game's code names those tables. The later build's songs name the
// tables of the module.
class Module
{
public:
    Module(CartImage& image, Build build, uint32_t base, uint32_t samples, uint32_t masks)
        : image_(image),
          build_(build),
          base_(base),
          samples_(samples),
          masks_(masks),
          pattern_place_(base + kModulePatterns)
    {
        image_.Put8(masks_, kNoteInstrument);
        image_.Put8(masks_ + 1, kVolumeEffect);
        for (uint32_t i = 2; i < kTableMasks; i++)
        {
            image_.Put8(masks_ + i, kMaskNote);
        }
    }

    // A module with its sample table and table of masks inside it, after `base`.
    Module(CartImage& image, Build build, uint32_t base)
        : Module(image, build, base, base + kModuleSamples, base + kModuleMasks)
    {
    }

    void AddSample(const TestSample& s)
    {
        const uint32_t entry = samples_ + kSampleSize * sample_count_;
        const uint32_t points = base_ + kModulePoints + 0x1000 * sample_count_;
        image_.Put32(entry + kSamplePoints, points);
        image_.Put16(entry + kSampleFlags, s.flags);
        image_.Put16(entry + kSampleLoopStart, s.loop_start);
        image_.Put16(entry + kSampleLength, s.length);
        image_.Put16(entry + kSampleRate, s.rate);
        for (uint32_t i = 0; i < s.length; i++)
        {
            const int saw = int(i % 64) * 2 * s.level / 63 - s.level;
            image_.Put8(points + i, uint8_t(s.saw ? int8_t(std::clamp(saw, -128, 127)) : s.level));
        }

        sample_count_++;
    }

    // Adds a song whose orders play its patterns in turn, each pattern given as its rows.
    void AddSong(const TestSong& s)
    {
        const uint32_t song = songs_++;
        const uint32_t orders = base_ + kModuleOrders + 0x40 * song;
        const uint32_t table = base_ + kModulePatternTable + 0x100 * song;
        for (size_t p = 0; p < s.patterns.size(); p++)
        {
            image_.Put8(orders + uint32_t(p), uint8_t(p));
            image_.Put32(table + 4 * uint32_t(p), pattern_place_);
            WritePattern(s.patterns[p]);
        }

        image_.Put8(orders + uint32_t(s.patterns.size()), kOrderEnd);

        // The game's code names each song's header in a literal, as a game that plays a song does.
        const uint32_t h = Header(song);
        image_.Put32(base_ + kModuleSongLiterals + 4 * song, h);
        image_.Put32(h + kHeaderPatterns, table);
        image_.Put32(h + kHeaderOrders, orders);
        if (build_ == Build::k2002)
        {
            image_.Put8(h + kHeader2002Tempo, s.tempo);
            image_.Put8(h + kHeader2002Speed, s.speed);
        }
        else
        {
            image_.Put32(h + kHeader2003Samples, samples_);
            image_.Put32(h + kHeader2003Masks, masks_);
            image_.Put8(h + kHeader2003Tempo, s.tempo);
            image_.Put8(h + kHeader2003Speed, s.speed);
        }
    }

    // Returns the address of song `song`'s header.
    uint32_t Header(uint32_t song) const
    {
        return base_ + kModuleSongs + (build_ == Build::k2002 ? kHeader2002Size : kHeader2003Size) * song;
    }

    uint32_t SongCount() const
    {
        return songs_;
    }

private:
    void WritePattern(const std::vector<Row>& rows)
    {
        image_.Put8(pattern_place_++, uint8_t(rows.size()));
        for (const Row& row : rows)
        {
            for (const Entry& e : row)
            {
                // Mode 1 for the channel's last mask, the table's mode where the mask is there, or else mode 0 with
                // the mask.
                uint8_t mode = 0;
                if (e.last_mask)
                {
                    mode = 1;
                }
                else if (e.mask == kNoteInstrument)
                {
                    mode = 2;
                }
                else if (e.mask == kVolumeEffect)
                {
                    mode = 3;
                }

                image_.Put8(pattern_place_++, uint8_t((mode << 4) | (e.channel + 1)));
                if (mode == 0)
                {
                    image_.Put8(pattern_place_++, e.mask);
                }

                if (e.mask & kMaskNote)
                {
                    image_.Put8(pattern_place_++, e.note);
                }

                if (e.mask & kMaskInstrument)
                {
                    image_.Put8(pattern_place_++, e.instrument);
                }

                if (e.mask & kMaskVolume)
                {
                    image_.Put8(pattern_place_++, e.volume);
                }

                if (e.mask & kMaskEffect)
                {
                    image_.Put8(pattern_place_++, e.effect);
                    image_.Put8(pattern_place_++, e.parameter);
                }
            }
            image_.Put8(pattern_place_++, 0);
        }
    }

    CartImage& image_;
    Build build_;
    uint32_t base_;
    uint32_t samples_;
    uint32_t masks_;
    uint32_t sample_count_ = 0;
    uint32_t songs_ = 0;
    uint32_t pattern_place_;
};

// The places of a test cartridge's parts.
constexpr uint32_t kSteps = kRomBase + 0x1000;
constexpr uint32_t kFineUp = kSteps + 4 * kNotes;
constexpr uint32_t kFineDown = kFineUp + 2 * (64 + 256);
constexpr uint32_t kVibratoTable = kFineDown + 2 * (64 + 256);
constexpr uint32_t kCode = kRomBase + 0x200;
constexpr uint32_t kLiterals = kRomBase + 0x400;
constexpr uint32_t kHandler = kRomBase + 0x600;
constexpr uint32_t kInit = kRomBase + 0x800;
constexpr uint32_t kMusicVolume = kRomBase + 0x880;
constexpr uint32_t kInitCall = kRomBase + 0x900;
constexpr uint32_t kInitLiteral = kRomBase + 0xA00;

// The mixer's settings that the test cartridge's init gives by default: the 2002 build's rate, and the later build's
// reload of timer 0.
constexpr uint32_t kInitRate = 14000;
constexpr uint16_t kTimerReload = 0xFBE6;
constexpr uint32_t kSamples = kRomBase + kModuleSamples;
constexpr uint32_t kMasks = kRomBase + kModuleMasks;
constexpr uint32_t kSongs = kRomBase + kModuleSongs;

// The mark of the handler: the mixer's lead over the FIFOs at which it runs the mix routine. The games' marks: 128 in
// the 2002 build's game, 96 in the later build's games with a DMA's interrupt.
constexpr uint32_t Mark(Build build)
{
    return build == Build::k2002 ? 128 : 96;
}

// Writes the first five instructions of a game's master interrupt handler at `at`, in ARM, as the games have them
// (kMasterHandlerCode).
inline void PutMasterHandler(CartImage& image, uint32_t at)
{
    for (const uint32_t word : kMasterHandlerCode)
    {
        image.Put32(at, word);
        at += 4;
    }
}

// Returns the two halfwords of a Thumb `bl` at `at` to `target`.
inline std::vector<uint16_t> ThumbCall(uint32_t at, uint32_t target)
{
    const uint32_t offset = target - (at + 4);

    return {uint16_t(0xF000 | ((offset >> 12) & 0x7FF)), uint16_t(0xF800 | ((offset >> 1) & 0x7FF))};
}

// A cartridge with the driver's tables, the handler's test of its mark, the code of the init that detection reads and
// one module.
class TestCart
{
public:
    explicit TestCart(Build build) : image_(kModuleSize), module_(image_, build, kRomBase)
    {
        for (int n = 0; n < kNotes; n++)
        {
            image_.Put32(kSteps + 4 * uint32_t(n),
                         uint32_t(std::lround(kUnitStep * std::exp2((n - kMiddleNote) / 12.0))));
        }

        for (uint32_t i = 0; i < 64 + 256; i++)
        {
            const double fine = i < 64 ? i / 768.0 : (i - 64) / 192.0;
            const double scale = i < 64 ? 32768.0 : 16384.0;
            image_.Put16(kFineUp + 2 * i, uint16_t(std::lround(scale * std::exp2(fine))));
            image_.Put16(kFineDown + 2 * i, uint16_t(std::min(65535.0, std::round(65536.0 * std::exp2(-fine)))));
        }

        for (uint32_t i = 0; i < 64; i++)
        {
            image_.Put8(kVibratoTable + i, uint8_t(int8_t(std::lround(64 * std::sin(i * 2 * std::numbers::pi / 64)))));
        }

        // The handler's test of the mixer's lead.
        const uint32_t mark = Mark(build);
        if (build == Build::k2002)
        {
            // cmp r1, #mark; beq; bx r6
            image_.PutHalfwords(kHandler, {uint16_t(0x2900 | mark), 0xD000, 0x4730});
            AddCode2002();
        }
        else
        {
            // The crt0 copies an image of IWRAM that holds `cmp ip, #mark << 22; bxne r4`.
            image_.Put32(kRomBase + 0xC0 + 0x74 + 20, kHandler);
            image_.Put32(kRomBase + 0xC0 + 0x74 + 24, 0x03000000);
            image_.Put32(kRomBase + 0xC0 + 0x74 + 28, 8);
            image_.Put32(kHandler, 0xE35C0500 | mark);
            image_.Put32(kHandler + 4, 0x112FFF14);
        }

        AddInit(build);
    }

    // Sets the rate that the game passes to the 2002 build's init.
    void SetInitRate(uint32_t rate)
    {
        image_.Put32(kInitLiteral, rate);
    }

    // Sets the reload of timer 0 that the later build's init stores.
    void SetTimerReload(uint16_t reload)
    {
        image_.Put32(kInitLiteral, 0xFFFF0000u | reload);
    }

    void AddSample(const TestSample& s)
    {
        module_.AddSample(s);
    }

    void AddSong(const std::vector<std::vector<Row>>& patterns, uint8_t tempo, uint8_t speed)
    {
        module_.AddSong({patterns, tempo, speed});
    }

    void AddSong(const TestSong& song)
    {
        module_.AddSong(song);
    }

    // Writes `code` as the game code in the cartridge's header (CartImage::SetGameCode).
    void SetGameCode(std::string_view code)
    {
        image_.SetGameCode(code);
    }

    Rom ToRom() const
    {
        return image_.ToRom();
    }

private:
    // Writes the init that detection reads, and its call. The 2002 build: the init's first instructions, and
    // `ldr r0, =kInitRate; bl init`. The later build: the setter of the music volume's first instructions, and an init
    // that runs `push {r4, lr}; adds r4, r0, #0; ldr r1, =0xFFFF0000 | kTimerReload; strh r1, [r0, #0]; movs r1, #4;
    // bl volume`.
    void AddInit(Build build)
    {
        const auto literal_load = [](uint32_t reg, uint32_t at)
        {
            return uint16_t(0x4800 | (reg << 8) | ((kInitLiteral - ((at + 4) & ~3u)) / 4));
        };

        if (build == Build::k2002)
        {
            image_.PutHalfwords(kInit, {0xB5F8, 0x4C00, 0x2300, 0x3C18, 0x60A3, 0x0400, 0x0C00, 0x60E3});
            image_.Put16(kInitCall, literal_load(0, kInitCall));
            image_.PutHalfwords(kInitCall + 2, ThumbCall(kInitCall + 2, kInit));
            SetInitRate(kInitRate);
            return;
        }

        image_.PutHalfwords(kMusicVolume, {0xB430, 0x2410, 0x2200, 0x0609, 0x0E09, 0x0053});
        image_.PutHalfwords(kInit, {0xB510, 0x1C04, literal_load(1, kInit + 4), 0x8001, 0x2104});
        image_.PutHalfwords(kInit + 10, ThumbCall(kInit + 10, kMusicVolume));
        SetTimerReload(kTimerReload);
    }

    // Writes the code of the 2002 build that names its sample table and its table of masks.
    void AddCode2002()
    {
        // lsls r3, r4, #1; adds r3, r3, r4; ldr r4, [pc, #imm]; lsls r3, r3, #2
        const uint32_t sample_load = kCode + 4;
        image_.PutHalfwords(kCode,
                            {0x0063, 0x191B, uint16_t(0x4C00 | ((kLiterals - ((sample_load + 4) & ~3u)) / 4)), 0x009B});
        image_.Put32(kLiterals, kSamples);
        // ldr r5, [pc, #imm]; ldr r2, [r1, #16]; ldrb r0, [r2]; adds r2, #1
        const uint32_t mask_load = kCode + 0x20;
        image_.PutHalfwords(
            mask_load, {uint16_t(0x4D00 | ((kLiterals + 4 - ((mask_load + 4) & ~3u)) / 4)), 0x690A, 0x7810, 0x3201});
        image_.Put32(kLiterals + 4, kMasks);
    }

    CartImage image_;
    Module module_;
};

// The effects, by Impulse Tracker's letters, and the note delay.
constexpr uint8_t kEffectA = 1;
constexpr uint8_t kEffectB = 2;
constexpr uint8_t kEffectC = 3;
constexpr uint8_t kEffectD = 4;
constexpr uint8_t kEffectE = 5;
constexpr uint8_t kEffectF = 6;
constexpr uint8_t kEffectG = 7;
constexpr uint8_t kEffectH = 8;
constexpr uint8_t kEffectK = 11;
constexpr uint8_t kEffectL = 12;
constexpr uint8_t kEffectO = 15;
constexpr uint8_t kEffectQ = 17;
constexpr uint8_t kEffectT = 20;
constexpr uint8_t kEffectU = 21;
constexpr uint8_t kEffectX = 24;
constexpr uint8_t kNoteDelay = 27;

// The samples of the crafted module: a forward loop, a ping-pong loop, a one-shot sample and a short ping-pong loop.
inline std::vector<TestSample> CraftedSamples()
{
    return {
        {.level = 60, .length = 1000, .flags = kSampleLoops, .loop_start = 200, .rate = 16000, .saw = true},
        {.level = 50,
         .length = 900,
         .flags = kSampleLoops | kSamplePingPong,
         .loop_start = 300,
         .rate = 12000,
         .saw = true},
        {.level = 70, .length = 3000, .flags = 0, .loop_start = 0, .rate = 22050, .saw = true},
        {.level = 40,
         .length = 504,
         .flags = kSampleLoops | kSamplePingPong,
         .loop_start = 500,
         .rate = 8000,
         .saw = true},
    };
}

// The songs of the crafted module. They play each effect of the driver's table with its parameters' cases, a
// parameter of 0 that takes the channel's last one, the masks of the entries' modes and their bits that repeat the
// channel's last values, and the samples' loops at pitches from low to high.
inline std::vector<TestSong> CraftedSongs()
{
    // A pattern of 16 rows with the given entries in the given rows.
    const auto rows = [](std::initializer_list<std::pair<int, Row>> entries, size_t count = 16)
    {
        std::vector<Row> p(count);
        for (const auto& [row, entry] : entries)
        {
            p[size_t(row)] = entry;
        }

        return p;
    };
    const auto fx = [](int channel, uint8_t effect, uint8_t parameter)
    {
        return EffectEntry(channel, effect, parameter);
    };
    const auto nfx =
        [](int channel, uint8_t note, uint8_t instrument, uint8_t volume, uint8_t effect, uint8_t parameter)
    {
        return NoteEffectEntry(channel, note, instrument, volume, effect, parameter);
    };

    std::vector<TestSong> songs;

    // Song 0: the volume slides, the pitch slides and the vibratos, each with a parameter of 0 after it.
    songs.push_back(
        {{rows({{0,
                 {NoteEntry(0, 60, 0, 48), NoteEntry(1, 64, 1, 40), NoteEntry(2, 55, 0, 50), NoteEntry(3, 67, 1, 30),
                  NoteEntry(4, 48, 0, 64)}},
                {1, {fx(0, kEffectD, 0x04), fx(1, kEffectE, 0x08), fx(2, kEffectH, 0x46), fx(3, kEffectU, 0x84)}},
                {2, {fx(0, kEffectD, 0x00), fx(1, kEffectF, 0x10), fx(2, kEffectH, 0x00), fx(3, kEffectU, 0x00)}},
                {3, {fx(0, kEffectD, 0x30), fx(1, kEffectE, 0xF3), fx(2, kEffectK, 0x02), fx(4, kEffectD, 0x0F)}},
                {4, {fx(0, kEffectD, 0x00), fx(1, kEffectF, 0xE5), fx(2, kEffectK, 0x00), fx(4, kEffectD, 0xF2)}},
                {5, {fx(0, kEffectD, 0x2F), fx(1, kEffectE, 0x00), fx(2, kEffectK, 0x30), fx(4, kEffectD, 0x00)}},
                {6, {fx(0, kEffectD, 0xF3), fx(1, kEffectF, 0x00), fx(2, kEffectK, 0x4F), fx(4, kEffectD, 0xF0)}},
                {7, {fx(0, kEffectD, 0xFF), fx(1, kEffectF, 0xF0), fx(2, kEffectK, 0xF2), fx(4, kEffectD, 0x0F)}},
                {8, {fx(1, kEffectE, 0xE0), fx(2, kEffectH, 0x0F), fx(3, kEffectH, 0xF1), fx(4, kEffectK, 0x01)}},
                {9, {nfx(0, 72, 1, 20, kEffectD, 0x00), fx(2, kEffectU, 0x00), fx(4, kEffectK, 0x10)}},
                {10, {fx(0, kEffectK, 0x0F), fx(1, kEffectF, 0x7F), fx(2, kEffectD, 0xF0)}},
                {11, {fx(0, kEffectK, 0xF1), fx(1, kEffectE, 0x7F), fx(2, kEffectD, 0x0F), fx(4, kEffectC, 0x01)}},
                {12, {nfx(0, 72, 1, 64, kEffectK, 0x40), fx(2, kEffectH, 0x88)}},
                {13, {fx(0, kEffectH, 0x00), fx(1, kEffectE, 0xFF)}},
                {14, {fx(0, kEffectK, 0x00), fx(1, kEffectF, 0xEF)}}})},
         110,
         4});

    // Song 1: the slides to a note, alone and with the volume slides, up and down, past their targets and with a
    // parameter of 0.
    songs.push_back(
        {{rows(
             {{0,
               {NoteEntry(0, 48, 0, 40), NoteEntry(1, 72, 1, 40), NoteEntry(2, 60, 0, 40), NoteEntry(3, 60, 1, 40),
                NoteEntry(4, 50, 0, 64)}},
              {1,
               {nfx(0, 60, 0, 50, kEffectG, 0x10), nfx(1, 60, 1, 30, kEffectG, 0x08), nfx(2, 72, 0, 20, kEffectL, 0x04),
                nfx(3, 48, 1, 60, kEffectL, 0x40), fx(4, kEffectL, 0x02)}},
              {2, {fx(0, kEffectG, 0x00), fx(1, kEffectG, 0x00), fx(2, kEffectL, 0x00), fx(3, kEffectL, 0x00)}},
              {3,
               {nfx(0, 36, 0, 64, kEffectG, 0xFF), nfx(1, 84, 1, 64, kEffectG, 0xFF), nfx(2, 40, 0, 20, kEffectL, 0x40),
                nfx(3, 80, 1, 60, kEffectL, 0x04), nfx(4, 62, 0, 30, kEffectL, 0x2F)}},
              {4, {fx(2, kEffectL, 0x00), fx(3, kEffectL, 0x00), fx(4, kEffectL, 0xF1)}},
              {5,
               {nfx(0, 55, 0, 32, kEffectL, 0x0F), nfx(1, 70, 1, 32, kEffectL, 0xF0), fx(2, kEffectG, 0x00),
                nfx(3, 255, 1, 0, kEffectL, 0x08), nfx(4, 50, 0, 40, kEffectL, 0x40)}},
              {6, {fx(0, kEffectL, 0x00), fx(1, kEffectL, 0x00), NoteEntry(3, 60, 1, 50), fx(4, kEffectL, 0x00)}},
              {7, {nfx(3, 72, 1, 40, kEffectG, 0x00), fx(4, kEffectL, 0x08)}},
              {8, {nfx(0, 60, 0, 10, kEffectL, 0x40), nfx(2, 50, 0, 64, kEffectL, 0x08)}},
              {9, {nfx(1, 255, 1, 0, kEffectG, 0x10), fx(2, kEffectL, 0x00)}},
              {10, {fx(1, kEffectG, 0x00), nfx(4, 30, 0, 64, kEffectG, 0x40)}},
              {11, {nfx(0, 70, 0, 50, kEffectL, 0x60), nfx(2, 70, 0, 2, kEffectL, 0x10)}},
              {12, {nfx(0, 40, 0, 50, kEffectL, 0x06), nfx(2, 30, 0, 62, kEffectL, 0x01)}}})},
         125,
         6});

    // Song 2: the retriggers, the note delays, the offsets, the note offs, the pans and the masks' modes and bits.
    Entry last_note = {1, kMaskLastNote | kMaskLastInstrument | kMaskLastVolume, 0, 0, 0, 0, 0};
    Entry last_all = {2, kMaskLastNote | kMaskLastInstrument | kMaskLastVolume | kMaskLastEffect, 0, 0, 0, 0, 0};
    Entry repeat_mask = {3, kMaskNote | kMaskInstrument | kMaskVolume, 62, 2, 50, 0, 0, true};
    Entry table_note = {4, kNoteInstrument, 57, 2, 0, 0, 0};
    Entry table_volume = {4, kVolumeEffect, 0, 0, 33, kEffectX, 0x30};
    songs.push_back(
        {{rows({{0,
                 {nfx(0, 60, 0, 40, kEffectQ, 0x03), nfx(1, 64, 1, 40, kEffectX, 0xC0),
                  nfx(2, 67, 0, 40, kEffectO, 0x02), NoteEntry(3, 55, 2, 64), table_note}},
                {1, {fx(0, kEffectQ, 0x00), last_note, nfx(2, 255, 0, 0, kEffectX, 0x40), repeat_mask}},
                {2, {nfx(0, 72, 1, 50, kNoteDelay, 0x02), nfx(1, 52, 3, 60, kEffectX, 0x00), last_all, table_volume}},
                {3, {nfx(0, 60, 0, 40, kEffectQ, 0x81), fx(1, kEffectX, 0x80), repeat_mask}},
                {4,
                 {nfx(0, 60, 0, 40, kEffectO, 0x00), nfx(2, 255, 0, 0, kNoteDelay, 0x01),
                  nfx(3, 50, 1, 64, kEffectQ, 0x02)}},
                {5, {nfx(0, 64, 2, 40, kEffectO, 0x05), last_note, fx(3, kEffectQ, 0x00), table_note}},
                {6, {nfx(1, 255, 1, 0, kEffectQ, 0x01), nfx(4, 76, 2, 64, kEffectX, 0x3F)}},
                {7, {nfx(0, 48, 0, 64, kNoteDelay, 0x07), nfx(2, 60, 2, 64, kEffectO, 0x01)}},
                {8, {nfx(1, 70, 0, 64, kEffectX, 0xFF), fx(4, kEffectX, 0x01)}},
                {9, {fx(0, kEffectA, 0x03), last_all}},
                {12, {fx(0, kEffectT, 0x50), nfx(2, 84, 1, 64, kEffectO, 0x03)}},
                {14, {fx(0, kEffectT, 0xC8), fx(1, kEffectB, 0x01)}}}),
          rows({{0,
                 {nfx(0, 96, 1, 64, kEffectX, 0x40), nfx(1, 70, 3, 64, kEffectX, 0x40),
                  nfx(2, 108, 1, 64, kEffectX, 0x40), nfx(3, 24, 2, 64, kEffectX, 0x40),
                  nfx(4, 119, 0, 64, kEffectX, 0x40)}},
                {8,
                 {nfx(0, 12, 1, 64, kEffectX, 0xC0), nfx(1, 0, 3, 64, kEffectX, 0xC0),
                  nfx(2, 36, 2, 64, kEffectX, 0xC0)}},
                {15, {fx(0, kEffectB, 0x00)}}})},
         150,
         3});

    // Song 3: many voices of the loops at high pitches, loud enough to clip, then a jump back to an earlier order. The
    // short ping-pong loop plays only at low pitches: a step longer than a loop hangs the driver's mixer.
    Row chord;
    for (int c = 0; c < kChannels; c++)
    {
        chord.push_back(nfx(c, uint8_t(60 + 4 * c), uint8_t(c % 3), 64, kEffectX, uint8_t(c % 2 ? 0x40 : 0xC0)));
    }

    songs.push_back({{rows({{0, chord}, {8, {fx(0, kEffectE, 0x20), fx(1, kEffectF, 0x20)}}}),
                      rows({{0, {fx(2, kEffectH, 0xFF), fx(3, kEffectU, 0xFF)}}, {15, {fx(0, kEffectB, 0x00)}}})},
                     200,
                     2});

    return songs;
}

} // namespace agblib::testing::webfoot_cart
