// SPDX-License-Identifier: MIT

// Unit tests for ITE Media's driver: detection, the reader of songs and the walk of a song's rows. They build small
// synthetic cartridges in memory. A test reads no game's ROM.

#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/ite/driver.h"
#include "agblib/testing/agb_cart.h"

namespace agblib::ite
{
namespace
{

using agblib::testing::agb_files::CartImage;

// The cartridge's layout: the driver's init and its literal pool, the game's call of the init, and the bank.
constexpr uint32_t kInit = 0x08000100;
constexpr uint32_t kPool = 0x08000110;
constexpr uint32_t kCall = 0x08000180;
constexpr uint32_t kBankLiteral = 0x08000190;
constexpr uint32_t kBank = 0x08001000;
constexpr size_t kCartSize = 0x10000;

// A tempo whose tick lasts 256 samples, and one whose tick lasts 512.
constexpr int kTempo = 0xA2;
constexpr int kSlowTempo = 0x51;
constexpr uint64_t kTick = 256;

// A pattern: its rows, the events of channel 0's track and those of the other channels' tracks.
struct Pattern
{
    int rows = 0;
    std::vector<uint8_t> track;
    std::vector<std::vector<uint8_t>> more = {};
};

// A song. Each pattern has a track for each of its channels. `header_channels` replaces the count in the header when
// it isn't 0.
struct TestSong
{
    std::vector<uint8_t> orders;
    int restart = 0;
    int speed = 1;
    int tempo = kTempo;
    std::vector<Pattern> patterns;
    int channels = 1;
    int header_channels = 0;
};

// The literal pool of the driver's init: DMA3SAD, then each copy's source, destination and control.
constexpr uint32_t kPoolWords[] = {0x040000D4, 0x08000200, 0x03000A50, 0x84000030, 0x08000300, 0x030003F0, 0x8400003A};

// Writes the driver's init, its pool and the game's call of it, which loads the bank's address into r1.
void PutDriver(CartImage& cart, uint32_t bank, const uint32_t (&pool)[7] = kPoolWords)
{
    cart.PutHalfwords(kInit, {0xB5F0, 0x4903, 0xBDF0}); // push {r4-r7, lr}; ldr r1, [pc, #12]; pop {r4-r7, pc}
    for (uint32_t i = 0; i < 7; i++)
    {
        cart.Put32(kPool + 4 * i, pool[i]);
    }

    cart.PutHalfwords(kCall, {0x4903}); // ldr r1, [pc, #12]
    cart.PutBl(kCall + 2, kInit);
    cart.Put32(kBankLiteral, bank);
}

// Writes a bank of `songs` at kBank, with one sample entry.
void PutBank(CartImage& cart, const std::vector<TestSong>& songs)
{
    cart.Put16(kBank + kBankSongs, uint16_t(songs.size()));
    cart.Put16(kBank + kBankSamples, 1);
    cart.Put32(kBank + kBankSampleTable, 0x400);

    uint32_t place = 0x800;
    for (size_t i = 0; i < songs.size(); i++)
    {
        const TestSong& song = songs[i];
        cart.Put32(kBank + kBankSongTable + 4 * uint32_t(i), place);
        const uint32_t header = kBank + place;
        const int channels = song.header_channels ? song.header_channels : song.channels;
        const uint16_t fields[] = {uint16_t(song.orders.size()), uint16_t(song.restart),
                                   uint16_t(channels),           uint16_t(song.speed),
                                   uint16_t(song.tempo),         uint16_t(song.patterns.size())};
        for (uint32_t f = 0; f < 6; f++)
        {
            cart.Put16(header + 2 * f, fields[f]);
        }

        // The tables, then the tracks after them.
        const uint32_t tracks = header + kSongHeaderSize;
        const uint32_t order_list = tracks + 4 * uint32_t(song.patterns.size() * size_t(song.channels));
        const uint32_t rows = order_list + uint32_t(song.orders.size());
        cart.PutBytes(order_list, song.orders);
        uint32_t track = (rows + uint32_t(song.patterns.size()) + 3) & ~3u;
        for (size_t p = 0; p < song.patterns.size(); p++)
        {
            const Pattern& pattern = song.patterns[p];
            cart.Put8(rows + uint32_t(p), uint8_t(pattern.rows));
            for (int c = 0; c < song.channels; c++)
            {
                const std::vector<uint8_t>& events = c == 0 ? pattern.track : pattern.more[size_t(c - 1)];
                cart.Put32(tracks + 4 * uint32_t(p * size_t(song.channels) + size_t(c)), track - kBank);
                cart.PutBytes(track, events);
                track += uint32_t(events.size());
            }
        }

        place = (track - kBank + 3) & ~3u;
    }
}

agblib::agb::Rom MakeRom(const std::vector<TestSong>& songs)
{
    CartImage cart(kCartSize);
    PutDriver(cart, kBank);
    PutBank(cart, songs);
    return cart.ToRom();
}

Flow Walk(const TestSong& test_song, uint64_t max_samples = 1u << 20)
{
    const agblib::agb::Rom rom = MakeRom({test_song});
    Song song;
    SEQLIB_CHECK(ReadSong(rom, kBank, 0, song));
    return WalkSong(rom, kBank, song, max_samples);
}

// A note that waits a row, and the events of command F.
const std::vector<uint8_t> kNoteAndRest = {1, kEventNote, 0x30};
const std::vector<uint8_t> kF00 = {0, kEventCommand | kEventParameter, kCommandSpeed, 0x00};

void TestDetection()
{
    const agblib::agb::Rom rom = MakeRom({{.orders = {0}, .patterns = {{2, kF00}}}, {.orders = {0}}});

    DriverInfo info;
    std::string error;
    const bool found = DetectDriver(rom, info, error);

    SEQLIB_CHECK(found);
    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK_EQ(info.init, kInit);
    SEQLIB_CHECK_EQ(info.init_call, kCall + 2);
    SEQLIB_CHECK_EQ(info.bank, kBank);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK_EQ(info.sample_count, 1);
}

void TestNoDriver()
{
    CartImage cart(kCartSize);
    PutBank(cart, {{.orders = {0}, .patterns = {{2, kF00}}}});
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(!DetectDriver(cart.ToRom(), info, error));
    SEQLIB_CHECK(error.empty());
}

void TestUnreadableBank()
{
    // The call passes an address past the ROM's end.
    CartImage cart(kCartSize);
    PutDriver(cart, 0x08F00000);
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(!DetectDriver(cart.ToRom(), info, error));
    SEQLIB_CHECK(!error.empty());
}

void TestReadSong()
{
    const agblib::agb::Rom rom =
        MakeRom({{.orders = {0, 1, 0}, .restart = 1, .speed = 6, .tempo = 125, .patterns = {{64, kF00}, {32, kF00}}},
                 {.orders = {0}}});

    Song song;
    Song empty;
    Song past;
    const bool read = ReadSong(rom, kBank, 0, song);
    const bool read_empty = ReadSong(rom, kBank, 1, empty);
    const bool read_past = ReadSong(rom, kBank, 2, past);

    SEQLIB_CHECK(read);
    SEQLIB_CHECK_EQ(song.orders, 3);
    SEQLIB_CHECK_EQ(song.restart, 1);
    SEQLIB_CHECK_EQ(song.channels, 1);
    SEQLIB_CHECK_EQ(song.speed, 6);
    SEQLIB_CHECK_EQ(song.tempo, 125);
    SEQLIB_CHECK_EQ(song.patterns, 2);
    SEQLIB_CHECK_EQ(rom.U8(song.rows + 1), 32);
    SEQLIB_CHECK(read_empty);
    SEQLIB_CHECK_EQ(empty.patterns, 0);
    SEQLIB_CHECK(WalkSong(rom, kBank, empty, 1u << 20).kind == Flow::Kind::kUnknown);
    SEQLIB_CHECK(!read_past);
}

void TestBadOrder()
{
    // The order list names pattern 1 of a song with one pattern.
    const agblib::agb::Rom rom = MakeRom({{.orders = {0, 1}, .patterns = {{2, kF00}}}});
    Song song;

    SEQLIB_CHECK(!ReadSong(rom, kBank, 0, song));
}

void TestLoop()
{
    // Orders 0 and 1 of two rows each, back to order 1: the loop goes from 2 ticks in to 4.
    const Flow flow = Walk({.orders = {0, 1}, .restart = 1, .patterns = {{2, kNoteAndRest}, {2, kNoteAndRest}}});

    SEQLIB_CHECK(flow.kind == Flow::Kind::kLoop);
    SEQLIB_CHECK_EQ(flow.loop_order, 1);
    SEQLIB_CHECK_EQ(flow.loop_start, 2 * kTick);
    SEQLIB_CHECK_EQ(flow.loop_end, 4 * kTick);
    SEQLIB_CHECK_EQ(flow.notes, 3);
}

void TestEnd()
{
    // F00 on the first row ends the song after that row's tick.
    const Flow flow = Walk({.orders = {0}, .patterns = {{4, kF00}}});

    SEQLIB_CHECK(flow.kind == Flow::Kind::kEnd);
    SEQLIB_CHECK_EQ(flow.end, kTick);
}

void TestTempo()
{
    // F51 sets the tempo from the next tick. F00 follows on the next row.
    const std::vector<uint8_t> track = {0, kEventCommand | kEventParameter, kCommandSpeed, kSlowTempo,
                                        0, kEventCommand | kEventParameter, kCommandSpeed, 0x00};

    const Flow flow = Walk({.orders = {0}, .patterns = {{4, track}}});

    SEQLIB_CHECK(flow.kind == Flow::Kind::kEnd);
    SEQLIB_CHECK_EQ(flow.end, kTick + 2 * kTick);
}

void TestParameterReuse()
{
    // A05 leaves 5 in the parameter byte, and F without a parameter byte reads it: speed 5. A parameter without a
    // command then takes the channel's command, F, and ends the song after 5 more ticks.
    const std::vector<uint8_t> track = {
        0, kEventCommand | kEventParameter, 0x0A, 0x05, 0, kEventCommand, kCommandSpeed, 0, kEventParameter, 0x00};

    const Flow flow = Walk({.orders = {0}, .patterns = {{8, track}}});

    SEQLIB_CHECK(flow.kind == Flow::Kind::kEnd);
    SEQLIB_CHECK_EQ(flow.end, 7 * kTick);
}

void TestJumpAndBreak()
{
    // B02 skips order 1, whose pattern would play 8 rows before its F00.
    const std::vector<uint8_t> jump = {0, kEventCommand | kEventParameter, kCommandJump, 0x02};
    const std::vector<uint8_t> late_end = {6, 0, 0, kEventCommand | kEventParameter, kCommandSpeed, 0x00};
    const std::vector<uint8_t> brk = {0, kEventCommand, kCommandBreak};

    const Flow jumped = Walk({.orders = {0, 1, 2}, .patterns = {{4, jump}, {8, late_end}, {4, kF00}}});
    const Flow broken = Walk({.orders = {0, 1}, .patterns = {{4, brk}, {4, kF00}}});

    SEQLIB_CHECK(jumped.kind == Flow::Kind::kEnd);
    SEQLIB_CHECK_EQ(jumped.end, 2 * kTick);
    SEQLIB_CHECK(broken.kind == Flow::Kind::kEnd);
    SEQLIB_CHECK_EQ(broken.end, 2 * kTick);
}

void TestTwoChannels()
{
    // Each pattern has a track for each channel: pattern 1's second track holds the F00.
    const Flow flow = Walk(
        {.orders = {0, 1}, .patterns = {{2, kNoteAndRest, {kNoteAndRest}}, {2, kNoteAndRest, {kF00}}}, .channels = 2});

    SEQLIB_CHECK(flow.kind == Flow::Kind::kEnd);
    SEQLIB_CHECK_EQ(flow.end, 3 * kTick);
    SEQLIB_CHECK_EQ(flow.notes, 3);
}

void TestParameterCleared()
{
    // F06 clears the parameter byte after use, so a later F without a parameter byte reads 0 and ends the song.
    const std::vector<uint8_t> track = {
        0, kEventCommand | kEventParameter, kCommandSpeed, 0x06, 0, kEventCommand, kCommandSpeed};

    const Flow flow = Walk({.orders = {0}, .patterns = {{4, track}}});

    SEQLIB_CHECK(flow.kind == Flow::Kind::kEnd);
    SEQLIB_CHECK_EQ(flow.end, 7 * kTick);
}

void TestSpeedAndTempoLimit()
{
    // F1F sets the speed to 31 ticks a row. F20 sets the tempo to 32, a tick of 0xA200 / 32 samples.
    const std::vector<uint8_t> speed = {0, kEventCommand | kEventParameter, kCommandSpeed, 0x1F,
                                        0, kEventCommand | kEventParameter, kCommandSpeed, 0x00};
    const std::vector<uint8_t> tempo = {0, kEventCommand | kEventParameter, kCommandSpeed, 0x20,
                                        0, kEventCommand | kEventParameter, kCommandSpeed, 0x00};

    const Flow slow = Walk({.orders = {0}, .patterns = {{4, speed}}});
    const Flow fast = Walk({.orders = {0}, .patterns = {{4, tempo}}});

    SEQLIB_CHECK(slow.kind == Flow::Kind::kEnd && fast.kind == Flow::Kind::kEnd);
    SEQLIB_CHECK_EQ(slow.end, 32 * kTick);
    SEQLIB_CHECK_EQ(fast.end, kTick + kTickSamples / 0x20);
}

void TestLoopRule()
{
    // B02 skips order 1. Order 2's B01 then goes down to order 1, which the song hasn't entered: no loop. Order 1 goes
    // on to order 2 again, but above order 1: no loop either. Order 2's B01 then goes back to order 1: the loop.
    const std::vector<uint8_t> to_two = {0, kEventCommand | kEventParameter, kCommandJump, 0x02};
    const std::vector<uint8_t> to_one = {0, kEventCommand | kEventParameter, kCommandJump, 0x01};
    const std::vector<uint8_t> note = {0, kEventNote | kEventCommand | kEventParameter, 0x30, 0x00, 0x00};

    const Flow flow = Walk({.orders = {0, 1, 2}, .patterns = {{2, to_two}, {1, note}, {2, to_one}}});

    SEQLIB_CHECK(flow.kind == Flow::Kind::kLoop);
    SEQLIB_CHECK_EQ(flow.loop_order, 1);
    SEQLIB_CHECK_EQ(flow.loop_start, 2 * kTick);
    SEQLIB_CHECK_EQ(flow.loop_end, 4 * kTick);
}

void TestLimits()
{
    // The walk stops at its limit. A tempo over 0xA200 or of 0 gives no walk at all.
    const TestSong looping = {.orders = {0, 1}, .restart = 1, .patterns = {{2, kNoteAndRest}, {2, kNoteAndRest}}};
    const TestSong too_fast = {.orders = {0}, .tempo = 0xA201, .patterns = {{4, kF00}}};
    const TestSong no_tempo = {.orders = {0}, .tempo = 0, .patterns = {{4, kF00}}};

    const Flow stopped = Walk(looping, kTick);
    const Flow fast = Walk(too_fast);
    const Flow none = Walk(no_tempo);

    SEQLIB_CHECK(stopped.kind == Flow::Kind::kUnknown);
    SEQLIB_CHECK_EQ(stopped.end, kTick);
    SEQLIB_CHECK(fast.kind == Flow::Kind::kUnknown && fast.end == 0);
    SEQLIB_CHECK(none.kind == Flow::Kind::kUnknown && none.end == 0);
}

void TestRejects()
{
    // A pool whose first copy goes to EWRAM isn't the driver's. A song of 17 channels doesn't read.
    constexpr uint32_t kEwramPool[] = {0x040000D4, 0x08000200, 0x02000A50, 0x84000030,
                                       0x08000300, 0x030003F0, 0x8400003A};
    CartImage cart(kCartSize);
    PutDriver(cart, kBank, kEwramPool);
    PutBank(cart, {{.orders = {0}, .patterns = {{2, kF00}}}});
    const agblib::agb::Rom wide = MakeRom({{.orders = {0}, .patterns = {{2, kF00}}, .header_channels = 17}});
    DriverInfo info;
    std::string error;
    Song song;

    const bool found = DetectDriver(cart.ToRom(), info, error);
    const bool read = ReadSong(wide, kBank, 0, song);

    SEQLIB_CHECK(!found && error.empty());
    SEQLIB_CHECK(!read);
    SEQLIB_CHECK_EQ(song.channels, 0);
}

void TestTime()
{
    SEQLIB_CHECK(FramesOfSamples(kRingSamples) == double(kRingFrames));
    SEQLIB_CHECK(SecondsOfSamples(16382) > 0.99 && SecondsOfSamples(16382) < 1.0);
}

// Runs every test. Returns the exit code.
int RunTests()
{
    TestDetection();
    TestNoDriver();
    TestUnreadableBank();
    TestReadSong();
    TestBadOrder();
    TestLoop();
    TestEnd();
    TestTempo();
    TestParameterReuse();
    TestJumpAndBreak();
    TestTwoChannels();
    TestParameterCleared();
    TestSpeedAndTempoLimit();
    TestLoopRule();
    TestLimits();
    TestRejects();
    TestTime();

    return seqlib::testing::Summary("ite");
}

} // namespace
} // namespace agblib::ite

int main()
{
    return agblib::ite::RunTests();
}
