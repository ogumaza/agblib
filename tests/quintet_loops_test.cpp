// SPDX-License-Identifier: MIT

// Unit tests of the loops of Quintet's conversion: where a loop starts and ends, its levels, its seam,
// and the limit on a song's length. They build small synthetic cartridges in memory. No game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "agblib/agb/files.h"
#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/quintet/archive_player.h"
#include "agblib/quintet/commands.h"
#include "agblib/quintet/driver.h"
#include "agblib/quintet/instruments.h"
#include "agblib/quintet/sequencer.h"
#include "agblib/quintet/song.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/psg.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/quintet/convert.h"
#include "agblib/sequence_export/quintet/soundfont.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/quintet_cart.h"
#include "quintet_test_util.h"

namespace agblib::sequence_export::quintet
{
namespace
{

// Returns the tick of the last marker in `midi` whose text is `name`, or `missing` if there's none.
uint32_t MarkerTick(const MidiEvents& midi, const std::string& name, uint32_t missing)
{
    uint32_t found = missing;
    for (const auto& [tick, bytes] : midi.Find(0xFF))
    {
        if (bytes.size() > 3 && bytes[1] == 0x06 && std::string(bytes.begin() + 3, bytes.end()) == name)
        {
            found = uint32_t(tick);
        }
    }

    return found;
}

void TestLoopStarts()
{
    // Square 1 loops two notes from its start, and square 2 plays a note and then loops the same two notes. The loop
    // starts after square 2's first note, within a frame, where both channels are looping, lasts two notes, and plays
    // twice before the song ends.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).Note(4, 3).End(),
                      Channel().Bytes({0x7F, 90, 0x5E, 15}).Note(2, 3).Bytes({0xBF}).Note(0, 3).Note(4, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_starts");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double length = sum.loop_end - sum.loop_start;
    SEQLIB_CHECK(sum.ok && sum.loop_start > 0);
    SEQLIB_CHECK(std::fabs(sum.loop_start - length / 2) < 0.02);
    SEQLIB_CHECK(std::fabs(sum.seconds - (sum.loop_start + 2 * length)) < 0.001);
}

void TestLoopAfterTempoChange()
{
    // Square 1 plays two notes at tempo 200, slows to tempo 60 and loops two notes, and square 2 loops four notes from
    // its start. The first pass of square 2 starts at the faster tempo. The loop starts at the second pass, a loop
    // after square 1's loop point, and the song still plays it twice in full.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 200, 0x5E, 15})
                          .Note(0, 3)
                          .Note(2, 3)
                          .Bytes({0x7F, 60, 0xBF})
                          .Note(4, 3)
                          .Note(5, 3)
                          .End(),
                      Channel().Bytes({0x5E, 15, 0xBF}).Note(0, 3).Note(2, 3).Note(4, 3).Note(5, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_tempo");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double length = sum.loop_end - sum.loop_start;
    SEQLIB_CHECK(sum.ok && sum.loop_start > length);
    SEQLIB_CHECK(std::fabs(sum.seconds - (sum.loop_start + 2 * length)) < 0.001);
}

void TestLoopLengths()
{
    // Square 1 loops three notes and square 2 four, both from the start, and the wave channel plays two notes and ends.
    // The loop starts where the wave channel ends, after two notes, and lasts until both squares are back where they
    // started, twelve notes, and the song plays it twice. The driver rounds the notes to frames. The checks allow for
    // that.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).Note(2, 3).Note(4, 3).End(),
                      Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).Note(2, 3).Note(4, 3).Note(5, 3).End(),
                      Channel().Bytes({0x7F, 90, 0x5E, 15}).Note(7, 3).Note(9, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_lengths");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double note = sum.loop_start / 2;
    SEQLIB_CHECK(sum.ok && sum.loop_start > 0);
    SEQLIB_CHECK(std::fabs(sum.loop_end - sum.loop_start - 12 * note) < 0.1);
    SEQLIB_CHECK(std::fabs(sum.seconds - (sum.loop_start + 24 * note)) < 0.1);
}

// Square 1's loop sets octave 2 after its first note. The first time through, that note plays at the octave 3 set
// before the loop, and every time after at octave 2. The loop starts at the second pass, and the driver goes on
// repeating that pass. The song plays the first pass and then the loop twice. A loop that plays the same each time
// starts at its first pass.
void TestLoopAtSecondPass()
{
    for (const bool moves : {true, false})
    {
        Cart cart(Revision::kA);
        cart.Songs({Song({Channel()
                              .Bytes({0x7F, 90, 0x5E, 15, 0x4F, uint8_t(moves ? 3 : 2), 0xBF})
                              .Note(0, 3)
                              .Bytes({0x4F, 2})
                              .Note(4, 3)
                              .End()})});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        ConvertSettings opt = TempSettings(moves ? "second_pass" : "first_pass");

        const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

        // The keys of the notes from the loop's start on.
        const MidiEvents midi = ReadMidi(sum.midi_path);
        const uint32_t loop_start = MarkerTick(midi, "loopStart", ~0u);
        std::vector<int> keys;
        for (const auto& [tick, bytes] : midi.Find(0x90))
        {
            if (tick >= loop_start)
            {
                keys.push_back(bytes[1]);
            }
        }

        // At tempo 90, the two quarter notes take 2 × 96 × 37 / 90 = 78.9 frames. The driver plays them in 78.
        const double kPass = 78 * 280896.0 / 16777216;
        SEQLIB_CHECK(sum.ok);
        SEQLIB_CHECK(std::fabs(sum.loop_start - (moves ? kPass : 0)) < 0.001);
        SEQLIB_CHECK(std::fabs(sum.loop_end - sum.loop_start - kPass) < 0.001);
        SEQLIB_CHECK(std::fabs(sum.seconds - sum.loop_start - 2 * kPass) < 0.001);
        SEQLIB_CHECK(keys == std::vector<int>({60, 64, 60, 64}));
    }
}

// The noise channel rests, then loops a note and a rest. At tempo 82, the loop point falls partway through a frame. The
// driver drops the part of a frame it has counted when it loops. The note then spans a frame more or less in the first
// pass than in the second. With a volume fade that changes the registers in every frame, the two notes make two drums.
// They start alike, and the loop starts at the first pass. A note that plays at another volume in the second pass
// differs in a frame that both passes span, and the loop starts at the second pass.
void TestLoopNoiseEnds()
{
    for (const bool differs : {false, true})
    {
        Cart cart(Revision::kA);
        Channel noise;
        noise.Bytes({0x7F, 82, 0x5E, 15, 0xD2, 100, 0, 0, 60, 1}).Rest(1).Bytes({0xBF}).Note(12, 2);
        if (differs)
        {
            noise.Bytes({0x5E, 8});
        }

        cart.Songs({Song({{0xFF}, {0xFF}, {0xFF}, noise.Rest(2).End()})});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        ConvertSettings opt = TempSettings(differs ? "loop_noise_differs" : "loop_noise_ends");

        const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

        const double pass = sum.loop_end - sum.loop_start;
        SEQLIB_CHECK(sum.ok && sum.loop_start > 0);
        SEQLIB_CHECK((sum.loop_start < pass) != differs);
    }
}

// Square 1 plays C, and then loops a note that slides from the C before it to E, and a C. The slide takes 4 frames
// the first time through. With `differs`, the loop then sets 16 frames, and every later pass slides over 16. The
// slide's notes start alike in both passes, at the same tick, key and bend, and differ only in their later frames. The
// loop starts at the first pass when the slides match, and at the second when they don't.
void TestLoopSlideSpeed()
{
    for (const bool differs : {false, true})
    {
        Cart cart(Revision::kA);
        cart.Songs({Song({Channel()
                              .Bytes({0x7F, 90, 0x5E, 15, 0x4F, 3, 0xE5, 4})
                              .Note(0, 3)
                              .Bytes({0xBF, 0xD4})
                              .Note(4, 3)
                              .Bytes({0xE5, uint8_t(differs ? 16 : 4)})
                              .Note(0, 3)
                              .End()})});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        ConvertSettings opt = TempSettings(differs ? "loop_slide_differs" : "loop_slide_same");

        const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

        const double pass = sum.loop_end - sum.loop_start;
        SEQLIB_CHECK(sum.ok && sum.loop_start > 0);
        SEQLIB_CHECK((sum.loop_start < pass) != differs);
    }
}

// Returns true if the song's loop starts at its second pass, and checks that it has a loop and an intro.
bool LoopsAtSecondPass(const Cart& cart, const char* name)
{
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings(name);

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double pass = sum.loop_end - sum.loop_start;
    SEQLIB_CHECK(sum.ok && sum.loop_start > 0);
    return sum.loop_start >= pass;
}

// A channel plays a note, and then loops a second note and the first again. The second note has a setting the first
// time through that `change` turns off after it. The note starts alike in both passes and differs only in what the
// driver writes in its later frames. The loop starts at the first pass when `change` is empty, and at the second when
// it isn't. On square 1, the setting is a volume envelope from 100% down to 50% over 8 frames (D2), and the driver
// writes NR12 as it falls. On square 1 again, it is the hardware envelope's step (3E). NR12 sets the step at the note's
// start. On the wave channel, it is a volume envelope, and the driver writes NR32 as it falls.
void TestLoopLevels()
{
    struct Case
    {
        const char* name;
        int channel;
        std::vector<uint8_t> setup;
        std::vector<uint8_t> change;
    };
    const Case cases[] = {
        {"loop_envelope", 0, {0x5E, 15, 0xD2, 100, 100, 50, 1, 8}, {0xD3}},
        {"loop_envelope_step", 0, {0x5E, 15, 0x4E, 0x3E, 2}, {0x3E, 0}},
        {"loop_wave_envelope", 2, {0xDA, 0x00, 0x5E, 3, 0xD2, 100, 100, 50, 1, 8}, {0xD3}},
    };
    for (const Case& test : cases)
    {
        for (const bool changes : {false, true})
        {
            std::vector<uint8_t> setup = {0x7F, 90, 0x4F, 3};
            setup.insert(setup.end(), test.setup.begin(), test.setup.end());
            Channel channel;
            channel.Bytes(setup).Note(0, 3).Bytes({0xBF}).Note(4, 3);
            if (changes)
            {
                channel.Bytes(test.change);
            }

            std::vector<std::vector<uint8_t>> channels(size_t(test.channel), {0xFF});
            channels.push_back(channel.Note(0, 3).End());
            Cart cart(Revision::kA);
            cart.Songs({Song(channels)});

            const std::string name = std::string(test.name) + (changes ? "_differs" : "_same");
            SEQLIB_CHECK(LoopsAtSecondPass(cart, name.c_str()) == changes);
        }
    }
}

// Square 1 plays C, and then loops an E and a C with the hardware's sweep bending each note down. The sweep's clock
// isn't tied to the frames, and a pass doesn't last a whole number of its steps. Its steps then fall at other places
// in the frames from pass to pass, and the bends of the frames differ. The game plays each pass as the one before.
// The bends of a swept note aren't compared, and the loop starts at the first pass. When the loop turns the sweep off
// after the E, only the first E sweeps. NR10 then differs, and the loop starts at the second pass.
void TestLoopSweep()
{
    for (const bool once : {false, true})
    {
        Channel square;
        square.Bytes({0x7F, 90, 0x5E, 15, 0x4F, 3, 0x1F, 1, 0x0E, 7, 0x1E}).Note(0, 3).Bytes({0xBF}).Note(4, 3);
        if (once)
        {
            square.Bytes({0x1F, 0, 0x0E, 0});
        }

        Cart cart(Revision::kA);
        cart.Songs({Song({square.Note(0, 3).End()})});

        SEQLIB_CHECK(LoopsAtSecondPass(cart, once ? "loop_sweep_once" : "loop_sweep") == once);
    }
}

// Square 1 loops two quarter notes from its start, and square 2 the same after a rest of 56 ticks. At tempo 90, a frame
// is 90 of the MIDI file's ticks and a tick is 37. Square 2's loop point is therefore 2 MIDI ticks into frame 23, and
// square 1's is at the start of frame 0. Each loop drops the part of a frame that the channel has counted, 84 MIDI
// ticks for square 1 and 86 for square 2. Square 2's second pass therefore starts 2 ticks before the end of a loop from
// its loop point, and its first note would play twice at the seam. The loop starts at the second pass, where both
// squares start each pass as the one before.
void TestLoopSeam()
{
    Cart cart(Revision::kA);
    cart.Songs(
        {Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0x4F, 3, 0xBF}).Note(0, 3).Note(4, 3).End(),
               Channel().Bytes({0x5E, 15, 0x4F, 3}).Rest(5).Rest(10).Bytes({0xBF}).Note(7, 3).Note(11, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_seam");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    // Each square's notes from the loop's start to its end.
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const uint32_t loop_start = MarkerTick(midi, "loopStart", ~0u);
    const uint32_t loop_end = MarkerTick(midi, "loopEnd", 0);
    std::array<int, 2> notes = {};
    for (const int c : {0, 1})
    {
        for (const auto& [tick, bytes] : midi.Find(uint8_t(0x90 + c)))
        {
            notes[size_t(c)] += tick >= loop_start && tick < loop_end ? 1 : 0;
        }
    }

    // Square 1's first pass ends at frame 78, and the loop starts as much after that as square 2's loop point is into
    // the song, 2072 MIDI ticks, and lasts 78 frames.
    const double kFrame = 280896.0 / 16777216;
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(std::fabs(sum.loop_start - (78 + 2072 / 90.0) * kFrame) < 1e-6);
    SEQLIB_CHECK(std::fabs(sum.loop_end - sum.loop_start - 78 * kFrame) < 1e-6);
    SEQLIB_CHECK(notes == (std::array<int, 2>{2, 2}));
}

// Square 1 plays a note at volume 15, and then loops a note at volume 15 and one at 8. The first time through, the
// loop's first note has the level that the note before it left. A player that jumps back to the loop's start comes
// from the note at volume 8. The level is therefore written again there.
void TestLoopSettingsAgain()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 90, 0x5E, 15, 0x4F, 3})
                          .Note(0, 3)
                          .Bytes({0xBF, 0x5E, 15})
                          .Note(4, 3)
                          .Bytes({0x5E, 8})
                          .Note(7, 3)
                          .End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_again");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    const uint32_t loop_start = MarkerTick(midi, "loopStart", ~0u);
    std::vector<std::pair<uint32_t, int>> levels;
    for (const auto& [tick, bytes] : midi.Find(0xB0))
    {
        if (bytes[1] == cc::kExpression)
        {
            levels.emplace_back(uint32_t(tick), bytes[2]);
        }
    }

    // The setup's level of 0, the first note's, the same again at the loop's start, and then the note at volume 8's.
    SEQLIB_CHECK(sum.ok && loop_start > 0);
    SEQLIB_REQUIRE(levels.size() > 3);
    SEQLIB_CHECK(levels[2] == std::make_pair(loop_start, levels[1].second));
    SEQLIB_CHECK(levels[3].first > loop_start && levels[3].second < levels[2].second);
}

void TestHourLimit()
{
    // A song asked to play its loop, a whole note at tempo 6 or 384 × 37 / 6 = 2368 frames, a hundred times stops where
    // the model gives up, after 216000 frames of 280896 cycles at 16.78 MHz, and says so.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 6, 0x5E, 15, 0xBF}).Note(0, 0).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("hour");
    opt.loops = 100;

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const auto cut_off = [](const std::string& w)
    {
        return w.find("cut off after an hour") != std::string::npos;
    };
    SEQLIB_CHECK(sum.ok && std::any_of(sum.warnings.begin(), sum.warnings.end(), cut_off));
    SEQLIB_CHECK(std::fabs(sum.seconds - 216000 * 280896.0 / 16777216) < 0.1);
}

} // namespace

int RunTests()
{
    TestLoopStarts();
    TestLoopAfterTempoChange();
    TestLoopLengths();
    TestLoopAtSecondPass();
    TestLoopNoiseEnds();
    TestLoopSlideSpeed();
    TestLoopLevels();
    TestLoopSweep();
    TestLoopSeam();
    TestLoopSettingsAgain();
    TestHourLimit();

    return seqlib::testing::Summary("quintet_loops");
}

} // namespace agblib::sequence_export::quintet

int main()
{
    return agblib::sequence_export::quintet::RunTests();
}
