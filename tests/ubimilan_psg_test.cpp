// SPDX-License-Identifier: MIT

// Tests of ubimilan::Output on small synthetic cartridges: the driver's stores to the PSG's registers in its strict
// mode, for the squares, the wave, the noise, the modulation, the slides and legato. Each expected value follows a rule
// of the driver's code, worked out by hand. The comments give the rule.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/ubimilan_image.h"
#include "agblib/ubimilan/driver.h"
#include "agblib/ubimilan/output.h"
#include "agblib/ubimilan/sequencer.h"
#include "ubimilan_output_test_util.h"

namespace
{

namespace ubimilan = agblib::ubimilan;
using agblib::agb::RegisterWrite;
using agblib::testing::ubimilan_image::TestImage;
using ubimilan::Output;
using Instrument = TestImage::Instrument;

using namespace agblib::testing::ubimilan_output_tests;

void TestSquares()
{
    // The instrument's bytes, the velocity ORed into the envelope, and the frequency of the key's place: 36 keys down.
    // TestImage's frequency table gives place i the value 1000 + i.
    TestImage image;
    image.SetInstrument(0, 1, {{0x11, 0x80, 0x05, 0x80}});
    image.SetInstrument(1, 2, {{0x40, 0x03, 0xC0}});
    const int sequence = image.AddSequence({
        0x00, 0x00, 0xC0, 1,    0x00, 0x00, 0xC1, 2, 0x00, 0x00, // programs
        0x90, 60,   0x70, 0x00, 0x00,                            // square 1's key 60
        0x91, 72,   0x60, 0x02, 0x00,                            // square 2's key 72
        0x80, 61,   0,    0x02, 0x00,                            // another key's note-off does nothing
        0x80, 60,   0,    0x20, 0x00,                            // its key's note-off silences the channel
        0xFC,
    });

    image.AddPiece(0, {sequence});
    const Played played = Play(image, 8);
    const std::vector<RegisterWrite>& first = played.writes[0];

    // The piece's start silences the PSG's envelopes.
    SEQLIB_REQUIRE(first.size() >= 4);
    XSFLIB_CHECK(first[0].address == kRegNr11 && first[0].value == 0);
    XSFLIB_CHECK(first[1].address == kRegNr21 && first[1].value == 0);
    XSFLIB_CHECK(first[2].address == kRegNr31 && first[2].value == 0);
    XSFLIB_CHECK(first[3].address == kRegNr41 && first[3].value == 0);
    XSFLIB_CHECK(StoreTo(first, kRegNr10) == 0x11);
    XSFLIB_CHECK(first[5].address == kRegNr11 && first[5].value == (0x80 | 0x75 << 8));
    XSFLIB_CHECK(StoreTo(first, kRegNr13) == (0x80 << 8 | (1000 + 24)));
    XSFLIB_CHECK(first[7].address == kRegNr21 && first[7].value == (0x40 | 0x63 << 8));
    XSFLIB_CHECK(StoreTo(first, kRegNr23) == (0xC0 << 8 | (1000 + 36)));
    XSFLIB_CHECK(StoreTo(played.writes[3], kRegNr11) == -1);
    XSFLIB_CHECK(StoreTo(played.writes[6], kRegNr11) == 0);

    // A key's place is a byte: key 30 of square 1 reads place 250.
    TestImage low;
    low.SetInstrument(0, 0, {{0, 0, 0, 0x80}});
    low.Put16(agblib::testing::ubimilan_image::kFrequencyTable + 2 * 250, 0x123);
    low.AddPiece(0, {low.AddSequence({0x00, 0x00, 0x90, 30, 0x70, 0x20, 0x00, 0xFC})});
    XSFLIB_CHECK(StoreTo(Play(low, 1).writes[0], kRegNr13) == (0x80 << 8 | 0x123));
}

void TestWave()
{
    // The wave channel's keys are 12 down. A program that changes loads the wave's 2 banks of 16 bytes: 32 bytes for
    // each program from the wave table. A program that stays loads nothing.
    TestImage image;
    image.SetInstrument(2, 0, {{0x20, 0x80}});
    image.SetInstrument(2, 1, {{0x30, 0xC0}});
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x92, 48,   0x40, 0x00, 0x00, // program 0's key 48
        0xC2, 1,    0x00, 0x00,                   // program 1 loads its wave
        0xC2, 1,    0x00, 0x00,                   // and doesn't again
        0x92, 50,   0x20, 0x20, 0x00,             // program 1's key 50
        0xFC,
    });

    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);
    std::vector<RegisterWrite> writes = played.writes[0];
    writes.erase(writes.begin(), writes.begin() + 4);
    SEQLIB_REQUIRE(writes.size() == 3 + 2 * 17 + 3);
    XSFLIB_CHECK(writes[0].address == kRegNr30 && writes[0].value == 0xA0);
    XSFLIB_CHECK(writes[1].address == kRegNr31 && writes[1].value == (0x20 | 0x40 << 8));
    XSFLIB_CHECK(writes[2].address == kRegNr33 && writes[2].value == (0x80 << 8 | (1000 + 36)));
    XSFLIB_CHECK(writes[3].address == kRegNr30 && writes[3].value == 0x20);
    XSFLIB_CHECK(writes[20].address == kRegNr30 && writes[20].value == 0x60);

    // TestImage's wave table gives byte i the value 8 * i, and program 1's wave is 32 bytes on: past that table.
    XSFLIB_CHECK(writes[4].address == 0x04000090 && writes[4].size == 1);
    XSFLIB_CHECK(writes[37].address == kRegNr30 && writes[37].value == 0xA0);
    XSFLIB_CHECK(writes[38].address == kRegNr31 && writes[38].value == (0x30 | 0x20 << 8));
    XSFLIB_CHECK(writes[39].address == kRegNr33 && writes[39].value == (0xC0 << 8 | (1000 + 38)));
}

void TestNoise()
{
    // Channel 9's keys up to 11 play the noise from the noise table, with the velocity ORed into the entry's second
    // byte. A note-off silences nothing. TestImage's entry of key 0 is 0xC000F01E.
    TestImage image;
    const int sequence = image.AddSequence({0x00, 0x00, 0x99, 0, 0x03, 0x00, 0x00, 0x89, 0, 0, 0x20, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const Played played = Play(image, 1);
    const std::vector<RegisterWrite>& writes = played.writes[0];
    XSFLIB_CHECK(StoreTo(writes, kRegNr43) == 0xC000);
    SEQLIB_REQUIRE(writes.size() == 6);
    XSFLIB_CHECK(writes[4].address == kRegNr41 && writes[4].value == (0x1E | 0xF3 << 8));

    // Each key's entry takes 4 bytes. A note-off of the noise stores nothing.
    TestImage other;
    other.Put32(agblib::testing::ubimilan_image::kNoiseTable + 4 * 3, 0x8042A010);
    other.AddPiece(0, {other.AddSequence({0x00, 0x00, 0x99, 3, 0x05, 0x01, 0x00, 0x89, 3, 0, 0x20, 0x00, 0xFC})});
    const Played other_played = Play(other, 3);
    XSFLIB_CHECK(StoreTo(other_played.writes[0], kRegNr41) == (0x10 | 0xA5 << 8));
    XSFLIB_CHECK(StoreTo(other_played.writes[0], kRegNr43) == 0x8042);
    XSFLIB_CHECK(other_played.writes[2].empty());
}

void TestModulation()
{
    // A modulation waits for its delay, then goes up and down in steps of depth * 65,536 / (period * 2) a frame, up
    // past its depth and down past minus its depth. Each frame of the track runs it, the note's frame too. A frame's
    // frequency comes from the fine table, at the key's place times 100 plus the modulation's whole part. TestImage's
    // fine table gives each place its number. With a depth of 2 and a period of 1, the step is 1 a frame. Key 60 is at
    // place 24.
    TestImage image;
    Instrument instrument;
    instrument.bytes = {0, 0, 0, 0x80};
    instrument.modulation_delay = 1;
    instrument.modulation_depth = 2;
    instrument.modulation_period = 1;
    instrument.flags = 1;
    image.SetInstrument(0, 0, instrument);
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0x90, 60, 0x70, 0x40, 0x00, 0xFC})});
    const std::vector<FrameStore> expected = {
        {0, 0x8000 | (1000 + 24)}, // the note. The delay takes frames 0 and 1
        {2, 2401},
        {3, 2402},
        {4, 2403}, // past the depth: it turns
        {5, 2402},
        {6, 2401},
        // Frame 7: a modulation of 0 without a slide sets nothing
        {8, 2399},
        {9, 2398},
        {10, 2397}, // past minus the depth: it turns
        {11, 2398},
        {12, 2399},
        // Frame 13: nothing
        {14, 2401},
    };
    XSFLIB_CHECK(StoresTo(Play(image, 15), kRegNr13) == expected);

    // A note-off stops no modulation. A note of an instrument without one does. With a delay of 0, the note's frame
    // starts the wave at 0.
    TestImage other;
    instrument.modulation_delay = 0;
    other.SetInstrument(0, 0, instrument);
    Instrument plain;
    plain.bytes = {0, 0, 0, 0x80};
    other.SetInstrument(0, 1, plain);
    const int sequence = other.AddSequence({
        0x00, 0x00, 0x90, 60,   0x70, 0x01, 0x00, // the note, on frame 0
        0x80, 60,   0,    0x01, 0x00,             // its note-off, on frame 2
        0xC0, 1,    0x00, 0x00,                   // the plain instrument, on frame 4
        0x90, 62,   0x70, 0x20, 0x00,             // its note, on frame 4
        0xFC,
    });

    other.AddPiece(0, {sequence});
    const std::vector<FrameStore> stopped = {
        {0, 0x8000 | (1000 + 24)}, {1, 2401}, {2, 2402}, {3, 2403}, {4, 0x8000 | (1000 + 26)},
    };
    XSFLIB_CHECK(StoresTo(Play(other, 7), kRegNr13) == stopped);
}

void TestSlide()
{
    // A pitch mode's first note starts a slide at the key's place times 100. From then on, each frame of the track sets
    // the frequency from the fine table at the slide's place, the note's frame too. The next note starts at the last
    // key's frequency, and moves by (new place - place) / frames a frame up to the new place. A note in the slide's
    // middle starts from the fine table.
    TestImage image;
    Instrument instrument;
    instrument.bytes = {0, 0, 0, 0x80};
    instrument.slide_frames = 4;
    instrument.flags = 2;
    image.SetInstrument(0, 0, instrument);
    const int sequence = image.AddSequence({
        0x00,
        0x00,
        0x90,
        60,
        0x70,
        0x01,
        0x00, // key 60 on frame 0, the next command on frame 2
        0x90,
        62,
        0x70,
        0x01,
        0x00, // key 62 on frame 2, the next command on frame 4
        0x90,
        73,
        0x70,
        0x20,
        0x00, // key 73 on frame 4
        0xFC,
    });

    image.AddPiece(0, {sequence});
    const std::vector<FrameStore> expected = {
        {0, 0x8000 | (1000 + 24)}, // the first note's frequency
        {0, 2400},                 // and its slide's place
        {1, 2400},
        {2, 0x8000 | (1000 + 24)}, // the second note starts at the last key's frequency
        {2, 2450},                 // a move of 200 / 4 a frame
        {3, 2500},
        {4, 0x8000 | 2500}, // the third note starts at the slide's place
        {4, 2500 + 300},    // a move of (3700 - 2500) / 4 a frame
        {5, 2500 + 2 * 300},
        {6, 2500 + 3 * 300},
        {7, 2500 + 4 * 300},
        {8, 3700}, // the place stays there
    };
    XSFLIB_CHECK(StoresTo(Play(image, 9), kRegNr13) == expected);

    // A move rounds towards 0. A place short of the target moves on by the whole step, and the next frame goes back to
    // the target. From key 60 to key 61, in 3 frames: 33 a frame, past the target on the fourth.
    TestImage rounded;
    instrument.slide_frames = 3;
    rounded.SetInstrument(0, 0, instrument);
    rounded.AddPiece(0,
                     {rounded.AddSequence({0x00, 0x00, 0x90, 60, 0x70, 0x00, 0x00, 0x90, 61, 0x70, 0x20, 0x00, 0xFC})});
    const std::vector<FrameStore> rounded_stores = {
        {0, 0x8000 | (1000 + 24)}, {0, 0x8000 | (1000 + 24)}, {0, 2433}, {1, 2466}, {2, 2499}, {3, 2532}, {4, 2500},
    };
    XSFLIB_CHECK(StoresTo(Play(rounded, 5), kRegNr13) == rounded_stores);

    // The game's division by a slide's 0 frames gives 0: the place goes to the target at once.
    TestImage at_once;
    instrument.slide_frames = 0;
    at_once.SetInstrument(0, 0, instrument);
    at_once.AddPiece(0,
                     {at_once.AddSequence({0x00, 0x00, 0x90, 60, 0x70, 0x01, 0x00, 0x90, 62, 0x70, 0x20, 0x00, 0xFC})});
    const std::vector<FrameStore> at_once_stores = {
        {0, 0x8000 | (1000 + 24)}, {0, 2400}, {1, 2400}, {2, 0x8000 | (1000 + 24)}, {2, 2600}, {3, 2600},
    };
    XSFLIB_CHECK(StoresTo(Play(at_once, 4), kRegNr13) == at_once_stores);
}

void TestSlideRepeats()
{
    Instrument instrument;
    instrument.bytes = {0, 0, 0, 0x80};
    instrument.slide_frames = 4;
    instrument.flags = 2;
    const auto play = [&instrument](int first, int second, int third)
    {
        TestImage image;
        image.SetInstrument(0, 0, instrument);
        const std::vector<uint8_t> commands = {
            0x00, 0x00,
            0x90, uint8_t(first),
            0x70, 0x01,
            0x00, // on frame 0
            0x90, uint8_t(second),
            0x70, 0x01,
            0x00, // on frame 2
            0x90, uint8_t(third),
            0x70, 0x20,
            0x00, // on frame 4
            0xFC,
        };
        image.AddPiece(0, {image.AddSequence(commands)});
        return StoresTo(Play(image, 7), kRegNr13);
    };

    // The slide's target again, on the way up: a move of 0, and the place goes to the target at once.
    const std::vector<FrameStore> up = {
        {0, 0x8000 | (1000 + 24)},
        {0, 2400},
        {1, 2400},
        {2, 0x8000 | (1000 + 24)},
        {2, 2450},
        {3, 2500},
        {4, 0x8000 | 2500},
        {4, 2600},
        {5, 2600},
        {6, 2600},
    };
    XSFLIB_CHECK(play(60, 62, 62) == up);

    // The slide's target again, on the way down: a move of 0, and the place stays short of the target.
    const std::vector<FrameStore> down = {
        {0, 0x8000 | (1000 + 26)},
        {0, 2600},
        {1, 2600},
        {2, 0x8000 | (1000 + 26)},
        {2, 2550},
        {3, 2500},
        {4, 0x8000 | 2500},
        {4, 2500},
        {5, 2500},
        {6, 2500},
    };
    XSFLIB_CHECK(play(62, 60, 60) == down);

    // The first note's key again: the slide has arrived. The next note starts at the place, from the fine table.
    const std::vector<FrameStore> arrived = {
        {0, 0x8000 | (1000 + 24)},
        {0, 2400},
        {1, 2400},
        {2, 0x8000 | (1000 + 24)},
        {2, 2400},
        {3, 2400},
        {4, 0x8000 | 2400},
        {4, 2450},
        {5, 2500},
        {6, 2550},
    };
    XSFLIB_CHECK(play(60, 60, 62) == arrived);
}

void TestSlideEnds()
{
    // A note of an instrument without a pitch mode ends the slide. The next pitch mode's note starts a new one.
    Instrument sliding;
    sliding.bytes = {0, 0, 0, 0x80};
    sliding.slide_frames = 4;
    sliding.flags = 2;
    Instrument plain;
    plain.bytes = {0, 0, 0, 0x80};
    TestImage image;
    image.SetInstrument(0, 0, sliding);
    image.SetInstrument(0, 1, plain);
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x90, 60,   0x70, 0x01, 0x00, // the slide's note, on frame 0
        0xC0, 1,    0x00, 0x00,                   // the plain instrument, on frame 2
        0x90, 62,   0x70, 0x01, 0x00,             // its note
        0xC0, 0,    0x00, 0x00,                   // the slide's instrument, on frame 4
        0x90, 64,   0x70, 0x20, 0x00,             // its note
        0xFC,
    });

    image.AddPiece(0, {sequence});
    const std::vector<FrameStore> expected = {
        {0, 0x8000 | (1000 + 24)}, {0, 2400}, {1, 2400}, {2, 0x8000 | (1000 + 26)},
        {4, 0x8000 | (1000 + 28)}, {4, 2800}, {5, 2800},
    };
    XSFLIB_CHECK(StoresTo(Play(image, 6), kRegNr13) == expected);
}

void TestLegato()
{
    // A legato's note on a sounding channel keeps the channel going: a store of the new key's frequency without the
    // bytes that restart it. After a note-off, the next note restarts it.
    TestImage image;
    Instrument instrument;
    instrument.bytes = {0x11, 0x80, 0x05, 0x80};
    instrument.flags = 4;
    image.SetInstrument(0, 0, instrument);
    const int sequence = image.AddSequence({
        0x00, 0x00, 0x90, 60,   0x70, 0x01, 0x00, // the note, on frame 0
        0x90, 62,   0x70, 0x01, 0x00,             // goes on, on frame 2
        0x80, 62,   0,    0x01, 0x00,             // stops, on frame 4
        0x90, 64,   0x70, 0x20, 0x00,             // restarts, on frame 6
        0xFC,
    });

    image.AddPiece(0, {sequence});
    const Played played = Play(image, 8);
    XSFLIB_CHECK(StoreTo(played.writes[0], kRegNr13) == (0x8000 | (1000 + 24)));
    XSFLIB_CHECK(StoreTo(played.writes[2], kRegNr13) == 1000 + 26);
    XSFLIB_CHECK(StoreTo(played.writes[2], kRegNr11) == -1);
    XSFLIB_CHECK(StoreTo(played.writes[4], kRegNr11) == 0);
    XSFLIB_CHECK(StoreTo(played.writes[6], kRegNr13) == (0x8000 | (1000 + 28)));
    XSFLIB_CHECK(StoreTo(played.writes[6], kRegNr10) == 0x11);

    // With a pitch mode, the note goes on by a move of the slide, and stores no note's frequency.
    TestImage sliding;
    instrument.flags = 4 | 2;
    instrument.slide_frames = 4;
    sliding.SetInstrument(0, 0, instrument);
    sliding.AddPiece(0,
                     {sliding.AddSequence({0x00, 0x00, 0x90, 60, 0x70, 0x01, 0x00, 0x90, 62, 0x70, 0x20, 0x00, 0xFC})});
    const std::vector<FrameStore> moved = {
        {0, 0x8000 | (1000 + 24)}, {0, 2400}, {1, 2400}, {2, 2450}, {3, 2500},
    };
    XSFLIB_CHECK(StoresTo(Play(sliding, 4), kRegNr13) == moved);

    // The wave channel's test of the pitch mode takes any flag: without a slide, the note stores nothing at all. The
    // note's key still counts for a note-off.
    TestImage wave;
    Instrument wave_instrument;
    wave_instrument.bytes = {0x10, 0x80};
    wave_instrument.flags = 4;
    wave.SetInstrument(2, 0, wave_instrument);
    const int wave_sequence = wave.AddSequence({
        0x00,
        0x00,
        0x92,
        60,
        0x20,
        0x01,
        0x00, // the note, on frame 0
        0x92,
        62,
        0x20,
        0x01,
        0x00, // goes on, on frame 2
        0x82,
        62,
        0,
        0x20,
        0x00, // stops, on frame 4
        0xFC,
    });

    wave.AddPiece(0, {wave_sequence});
    const Played wave_played = Play(wave, 5);
    XSFLIB_CHECK(StoreTo(wave_played.writes[0], kRegNr33) == (0x8000 | (1000 + 48)));
    XSFLIB_CHECK(wave_played.writes[2].empty());
    XSFLIB_CHECK(StoreTo(wave_played.writes[4], kRegNr31) == 0);
}

} // namespace

int main()
{
    TestSquares();
    TestWave();
    TestNoise();
    TestModulation();
    TestSlide();
    TestSlideRepeats();
    TestSlideEnds();
    TestLegato();
    return xsflib::testing::Summary("Ubisoft Milan's PSG output");
}
