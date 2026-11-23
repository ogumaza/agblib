// SPDX-License-Identifier: MIT

// Unit tests of the loops of Rare's conversion: where a loop starts and ends, a bend over a loop, a track without a
// loop, and the limit on a tune's length. They build small synthetic cartridges in memory. No game ROM is needed.

#include "rare_test_util.h"

namespace agblib::sequence_export::rare
{
namespace
{

void TestLoops()
{
    // A track loops between controllers 102 and 103. The conversion plays the loop twice, and marks it.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Wait(480)
                   .Control(0, kCtrlLoopStart, 0)
                   .Wait(960)
                   .Control(0, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Wait(480)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 60, 100)
                   .Wait(480)
                   .Off(1, 60)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("loops");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    const auto notes = m.Find(0x91);
    SEQLIB_REQUIRE_EQ(notes.size(), 2);
    SEQLIB_CHECK_EQ(notes[0].tick, 480);
    SEQLIB_CHECK_EQ(notes[1].tick, 1440);

    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back({uint32_t(e.tick), std::string(e.bytes.begin() + 3, e.bytes.end())});
        }
    }

    SEQLIB_CHECK(markers.size() == 2 && markers[0] == std::make_pair(480u, std::string("loopStart")) &&
                 markers[1] == std::make_pair(1440u, std::string("loopEnd")));

    // The loop starts at 0.5 s and ends at 1.5 s of song time. The GBA plays song time a little slower.
    SEQLIB_CHECK(std::fabs(r.loop_start - 0.5 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.loop_end - 1.5 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.seconds - 2.5 * kTempoScale) < 1e-9);
}

// A track bends back to the centre where its loop starts, where it already is the first time through, and bends up
// later in the loop. A player that jumps back to the loop's start comes from the bend up, as the game does. The centre
// is therefore written again there.
void TestLoopBend()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Wait(480)
                   .Control(0, kCtrlLoopStart, 0)
                   .Wait(960)
                   .Control(0, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Wait(480)
                   .Control(1, kCtrlLoopStart, 0)
                   .Bend(1, 0x2000)
                   .On(1, 60, 100)
                   .Wait(480)
                   .Off(1, 60)
                   .Bend(1, 0x3000)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("loop_bend");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    using Events = std::vector<seqlib::midi::Event>;
    const Events kBends = {{0, {0xE1, 0x00, 0x40}},
                           {480, {0xE1, 0x00, 0x40}},
                           {960, {0xE1, 0x00, 0x60}},
                           {1440, {0xE1, 0x00, 0x40}},
                           {1920, {0xE1, 0x00, 0x60}}};
    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(ReadMidi(r.midi_path).Find(0xE1) == kBends);
}

void TestLoopStarts()
{
    // The first track loops 480 ticks from the start, the second 960 ticks from tick 480, and the third 240 ticks from
    // tick 240. Every track is looping from tick 480. The loop goes from there for the 960 ticks that each track's loop
    // divides, and the tune plays it twice. The middle track's loop starts last and is the longest. Neither the first
    // track's loop nor the last track's loop gives the same times.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Program(0, 5)
                   .Control(0, kCtrlLoopStart, 0)
                   .On(0, 60, 100)
                   .Wait(240)
                   .Off(0, 60)
                   .Wait(240)
                   .Control(0, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Wait(480)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 64, 100)
                   .Wait(480)
                   .Off(1, 64)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(2, 5)
                   .Wait(240)
                   .Control(2, kCtrlLoopStart, 0)
                   .On(2, 67, 100)
                   .Wait(120)
                   .Off(2, 67)
                   .Wait(120)
                   .Control(2, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_starts");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The loop goes from 0.5 s to 1.5 s of song time, and the tune ends at 2.5 s. The GBA plays song time a little
    // slower.
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(std::fabs(r.loop_start - 0.5 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.loop_end - 1.5 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.seconds - 2.5 * kTempoScale) < 1e-9);
}

void TestLoopLengths()
{
    // One track loops 720 ticks and the other 960, both from the start. The loop lasts until both are back where they
    // started, 2880 ticks or 3 s of song time, and the tune plays it twice.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Program(0, 5)
                   .Control(0, kCtrlLoopStart, 0)
                   .On(0, 60, 100)
                   .Wait(360)
                   .Off(0, 60)
                   .Wait(360)
                   .Control(0, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 64, 100)
                   .Wait(480)
                   .Off(1, 64)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_lengths");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The GBA plays song time a little slower.
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(std::fabs(r.loop_start) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.loop_end - 3 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.seconds - 6 * kTempoScale) < 1e-9);
}

void TestTrackWithoutLoop()
{
    // In a looping tune, the loop starts no earlier than the last command of a track without a loop, and a track that
    // ends in a long delay doesn't lengthen the tune. Track 0 sets the tempo and waits 9600 ticks, track 1 loops 960
    // ticks from the start, and track 2 plays a note from tick 480 to tick 960 and ends. The loop goes from tick 960 to
    // tick 1920, and the tune ends at tick 2880.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(9600).End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 60, 100)
                   .Wait(480)
                   .Off(1, 60)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat).Program(2, 5).Wait(480).On(2, 64, 100).Wait(480).Off(2, 64).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("without_loop");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // At 960 ticks a second of song time. The GBA plays song time a little slower.
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(std::fabs(r.loop_start - 1.0 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.loop_end - 2.0 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.seconds - 3.0 * kTempoScale) < 1e-9);
}

void TestHourLimit()
{
    // A tune asked to play its loop of 50 s a hundred times stops where the model gives up, after 216000 frames
    // (60:16.42). The loop starts half a second in. The limit therefore falls in the middle of a note: the tune's
    // length, its MIDI file and that note all end there.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Program(1, 5)
                   .Wait(480)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 60, 100)
                   .Wait(48000)
                   .Off(1, 60)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("hour");
    opt.loops = 100;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const auto cut_off = [](const std::string& w)
    {
        return w.find("cut off after an hour") != std::string::npos;
    };
    SEQLIB_CHECK(r.ok && std::any_of(r.warnings.begin(), r.warnings.end(), cut_off));
    SEQLIB_CHECK(r.seconds > 3610 && r.seconds < 3620);

    const MidiEvents m = ReadMidi(r.midi_path);
    uint64_t last = 0;
    for (const auto& track : m.tracks)
    {
        for (const auto& e : track.events)
        {
            last = std::max(last, e.tick);
        }
    }

    const auto offs = m.Find(0x81);
    SEQLIB_CHECK(last / 960.0 * kTempoScale < 3620);
    SEQLIB_CHECK(!offs.empty() && offs.back().tick / 960.0 * kTempoScale > 3610);
}

} // namespace

int RunTests()
{
    TestLoops();
    TestLoopBend();
    TestLoopStarts();
    TestLoopLengths();
    TestTrackWithoutLoop();
    TestHourLimit();

    return seqlib::testing::Summary("rare_loops");
}

} // namespace agblib::sequence_export::rare

int main()
{
    return agblib::sequence_export::rare::RunTests();
}
