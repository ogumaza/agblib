// SPDX-License-Identifier: MIT

// Unit tests for Krawall. They build small synthetic cartridges in memory. No game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "agb_conversion_test_util.h"
#include "agblib/agb/rom.h"
#include "agblib/krawall/archive_player.h"
#include "agblib/krawall/commands.h"
#include "agblib/krawall/driver.h"
#include "agblib/krawall/instruments.h"
#include "agblib/krawall/player.h"
#include "agblib/krawall/song.h"
#include "agblib/sequence_export/agb/midi.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"
#include "agblib/sequence_export/krawall/convert.h"
#include "agblib/sequence_export/krawall/performance.h"
#include "agblib/sequence_export/krawall/soundfont.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/agb_files.h"
#include "agblib/testing/krawall_cart.h"
#include "krawall_test_util.h"

namespace agblib::sequence_export::krawall
{
namespace
{

using agblib::testing::agb_tests::CheckFilesInMemory;

// Detection finds the player by its code, its tables from the code and the values in them, the mixer's settings, the
// game's calls of the library's settings and the modules, in the order of the game's table and then the one it doesn't
// list. A count for the modules applies to the game's table, and leaves out the one it doesn't list. A game with the
// version string of another build gets an error that names the build.
void TestDetection()
{
    Cart cart;
    const uint32_t first = cart.Module(2, 5, LoopingCells());
    const uint32_t second = cart.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}});
    const uint32_t unlisted = cart.Module(1, 4, {{0, 0, 37, 1, -1, -1, 0}}, false);
    const Rom rom = cart.ToRom();
    Cart other(false);
    other.PutString(kVersion, "$Id: Krawall $Id: version.h 8 2005-04-21 12:24:45Z seb $");
    const Rom other_rom = other.ToRom();
    const Rom blank = Cart(false).ToRom();
    DriverInfo info, counted_info, other_info, blank_info;
    std::string error, counted_error, other_error, blank_error;
    DriverOverrides one;
    one.module_count = 1;

    const bool found = DetectDriver(rom, DriverOverrides(), info, error);
    const bool counted = DetectDriver(rom, one, counted_info, counted_error);
    const bool found_other = DetectDriver(other_rom, DriverOverrides(), other_info, other_error);
    const bool found_blank = DetectDriver(blank, DriverOverrides(), blank_info, blank_error);

    SEQLIB_CHECK(found && error.empty());
    SEQLIB_CHECK_EQ(info.play, kPlay);
    SEQLIB_CHECK_EQ(info.samples, kSampleTable);
    SEQLIB_CHECK_EQ(info.instruments, kInstrumentTable);
    SEQLIB_CHECK_EQ(info.sine, kSineTable);
    SEQLIB_CHECK_EQ(info.periods, kPeriodTable);
    SEQLIB_CHECK_EQ(info.linear, kLinearTable);
    SEQLIB_CHECK_EQ(info.mix_rate, 16384);
    SEQLIB_CHECK_EQ(info.frame_samples, 276);
    SEQLIB_CHECK(info.stereo);
    SEQLIB_CHECK_EQ(info.quality, 0);
    SEQLIB_CHECK_EQ(info.master_volume, 0x40080);
    SEQLIB_CHECK_EQ(info.module_table, kModuleTable);
    SEQLIB_CHECK_EQ(info.table_count, 2);
    SEQLIB_CHECK(info.modules == std::vector<uint32_t>({first, second, unlisted}));
    SEQLIB_CHECK(counted && counted_info.modules == std::vector<uint32_t>({first}));
    SEQLIB_CHECK(!found_other && other_error ==
                                     "found Krawall, but not a build that ntdlib supports ($Id: Krawall $Id: "
                                     "version.h 8 2005-04-21 12:24:45Z seb $)");
    SEQLIB_CHECK(!found_blank && blank_error.empty());

    // Franklin's Great Adventures holds the ramp's first values in other data before the player's tables. The ramp
    // follows the sine. BFKE's ARM start-up code sets kragInit()'s argument after it loads the routine's address:
    // ldr r3, =kragInit|1; mov r0, #0; mov lr, pc; bx r3.
    Cart franklin;
    franklin.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}});
    for (uint32_t i = 0; i < 16; i++)
    {
        franklin.Put16(kRomBase + 0x800 + 2 * i, uint16_t(8 * i));
    }

    for (uint32_t i = 0; i < 6; i += 2)
    {
        franklin.Put16(kGameInit + i, 0);
    }

    franklin.Put32(kRomBase + 0x900, 0xE59F3008);
    franklin.Put32(kRomBase + 0x904, 0xE3A00000);
    franklin.Put32(kRomBase + 0x908, 0xE1A0E00F);
    franklin.Put32(kRomBase + 0x90C, 0xE12FFF13);
    franklin.Put32(kRomBase + 0x910, kInit | 1);

    const DriverInfo franklin_info = Detect(franklin.ToRom());

    SEQLIB_CHECK_EQ(franklin_info.ramp, kRampTable);
    SEQLIB_CHECK_EQ(franklin_info.square, kSquareTable);
    SEQLIB_CHECK(!franklin_info.stereo);
    SEQLIB_CHECK(franklin_info.warnings.empty());

    // Harry Potter and the Prisoner of Azkaban gives the music 12 times its options' default level of 10.
    Cart potter;
    potter.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}});
    potter.SetGameCode("BHTE");

    SEQLIB_CHECK_EQ(info.music_volume, 128);
    SEQLIB_CHECK_EQ(Detect(potter.ToRom()).music_volume, 120);

    // Shrek SuperSlam's pause calls kramSetMasterVol(0x40020), before its sound init in the ROM. The master volume is
    // the sound init's, from the call that follows the game's call of kragInit().
    Cart shrek;
    shrek.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}});
    shrek.Put16(kRomBase + 0xE0, 0x4803);
    shrek.PutBl(kRomBase + 0xE2, kMaster);
    shrek.Put32(kRomBase + 0xF0, 0x40020);

    SEQLIB_CHECK_EQ(Detect(shrek.ToRom()).master_volume, 0x40080);
}

// The player starts a note at the rate its period gives and the volume its sample or the volume column gives, a volume
// slide changes the mixer's volume on the ticks between rows, an offset starts a note within its sample, a pan sets the
// sides' volumes, and a portamento up lowers the period on the ticks between rows.
void TestPlayer()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Player player(rom, info, info.modules[0]);
    std::vector<MixChannel> first, second;
    std::vector<uint64_t> times;
    std::vector<int> periods;
    const auto record = [&](uint64_t time)
    {
        times.push_back(time);
        first.push_back(player.MixerOf(player.Channels()[0]));
        second.push_back(player.MixerOf(player.Channels()[1]));
        periods.push_back(player.Channels()[0].period);
    };
    player.OnTick(record);

    while (times.size() < 13)
    {
        player.Frame();
    }

    // The first tick comes a tick's samples after the start, and the rest a tick apart.
    SEQLIB_CHECK(player.Valid());
    SEQLIB_CHECK_EQ(times[0], kTickSamples);
    SEQLIB_CHECK_EQ(times[12] - times[0], 12 * kTickSamples);

    // Row 0: C-4 at volume 64, and E-4 at its sample's volume, 48, with a pan of -32: the left side gets three times
    // the right's level.
    SEQLIB_CHECK_EQ(first[0].inc, int32_t(kC4Rate << 2));
    SEQLIB_CHECK_EQ(first[0].volume, 64);
    SEQLIB_CHECK_EQ(second[0].volume, 48);
    SEQLIB_CHECK_EQ(second[0].pan, -32);
    SEQLIB_CHECK_EQ(second[0].left, (48 * 64 * 96) >> 13);
    SEQLIB_CHECK_EQ(second[0].right, (48 * 64 * 32) >> 13);

    // Row 1 slides the volume down by 4 on its second and third ticks.
    SEQLIB_CHECK_EQ(first[3].volume, 64);
    SEQLIB_CHECK_EQ(first[4].volume, 60);
    SEQLIB_CHECK_EQ(first[5].volume, 56);

    // Row 2 starts E-4 again 256 points into its sample.
    SEQLIB_CHECK(second[6].handle != second[5].handle);
    SEQLIB_CHECK_EQ(second[6].pos - second[6].start, 256);

    // Row 3 lowers the period by 64 on each of its ticks between rows, and the rate goes up with it.
    SEQLIB_CHECK_EQ(periods[9], 1712);
    SEQLIB_CHECK_EQ(periods[10], 1712 - 64);
    SEQLIB_CHECK_EQ(periods[11], 1712 - 128);
    SEQLIB_CHECK_EQ(first[11].inc, int32_t((14317456 / (1712 - 128)) << 2));
}

// The mixer plays a sample to its end, or to its loop's end and back to the loop's start, or with a loop back and
// forth, back to the loop's start and forwards again. A silent channel moves on by its guess. For a step of less than a
// point, the guess is a point a sample.
void TestMixer()
{
    Cart cart;
    cart.Module(4, 4,
                {{0, 0, 49, 1, -1, -1, 0}, {0, 1, 85, 2, -1, -1, 0}, {0, 2, 49, 3, -1, -1, 0}, {0, 3, 49, 2, 0x10}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Player player(rom, info, info.modules[0]);
    bool forward_in_loop = true, bidi_turned = false;
    uint32_t one_shot_status = 1;
    std::vector<uint32_t> silent_points;
    const auto check = [&](uint64_t)
    {
        const MixChannel& f = player.MixerOf(player.Channels()[0]);
        const MixChannel& b = player.MixerOf(player.Channels()[2]);
        const MixChannel& s = player.MixerOf(player.Channels()[3]);
        forward_in_loop = forward_in_loop && f.status == 1 && f.pos < f.end && f.pos >= f.start;
        bidi_turned = bidi_turned || b.inc < 0;
        one_shot_status = player.MixerOf(player.Channels()[1]).status;
        if (s.status == 1)
        {
            silent_points.push_back(s.pos - s.start);
        }
    };
    player.OnTick(check);

    for (int f = 0; f < 40; f++)
    {
        player.Frame();
    }

    SEQLIB_CHECK(forward_in_loop);
    SEQLIB_CHECK(bidi_turned);
    SEQLIB_CHECK_EQ(one_shot_status, 0);
    SEQLIB_CHECK(silent_points.size() >= 2 && silent_points[1] - silent_points[0] == kTickSamples);
}

// In the enhanced mode, a sample that loops back and forth plays its attack: the points before the loop sound as they
// are, and the loop turns at its start only once it plays backwards.
void TestEnhancedAttack()
{
    Cart cart;
    cart.Module(4, 4, {{0, 0, 49, 3, -1, -1, 0}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Player player(rom, info, info.modules[0]);
    player.MixEnhanced(true);
    std::vector<float> mix;
    for (int f = 0; f < 4; f++)
    {
        player.Frame();
        const std::vector<float>& left = player.EnhancedOutput(0);
        const std::vector<float>& right = player.EnhancedOutput(1);
        for (size_t i = 0; i < left.size(); i++)
        {
            mix.push_back(left[i] + (i < right.size() ? right[i] : 0.0f));
        }
    }

    // The sample's first points rise from the middle, as a sine does. The loop's last point is below the middle.
    const auto first = std::find_if(mix.begin(), mix.end(), [](float s) { return s != 0; });
    SEQLIB_CHECK(mix.end() - first >= 20);
    SEQLIB_CHECK(std::all_of(first, first + 20, [](float s) { return s > 0; }));
}

// A module holds two songs separated by +++: the first plays pattern 0, with a note on C-4, and the second pattern 1,
// with a note an octave up. Each converts on its own in the player's song mode. At its end it goes back to its start,
// and it never plays the other song.
void TestSongs()
{
    Cart cart;
    const uint32_t module = cart.Module(1, {{4, {{0, 0, 49, 1}}}, {4, {{0, 0, 61, 1}}}});
    cart.Songs(module, {{0}, {1}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_songs";

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{module, 0}, {module, 1}}));
    SEQLIB_CHECK(first.ok && second.ok && !first.silent && !second.silent);
    std::vector<std::set<int>> keys;
    for (const SongReport* s : {&first, &second})
    {
        std::set<int> played;
        for (const auto& e : ReadMidi(s->midi_path).Find(0x90))
        {
            played.insert(e.bytes[1]);
        }
        keys.push_back(played);
    }

    SEQLIB_CHECK(keys[0].size() == 1 && keys[1].size() == 1);
    SEQLIB_CHECK(!keys[0].empty() && !keys[1].empty() && *keys[0].begin() + 12 == *keys[1].begin());
}

// Eight rows per quarter note give a tempo of 126 BPM, within the target range of 105 to 210 BPM. Each player tick is
// one MIDI tick, and the tempo comes from the sample count for eight rows. Each channel's notes are on its MIDI
// channel, on the keys their rates give, with a program for each sample and start offset. The five-row loop, from the
// first row back to it, plays twice; the volume slide becomes CC11 and the portamento bends upwards. SoundFont presets
// preserve sample offsets and C-4 rates, and bidirectional loops contain both the forward and reverse samples.
void TestConversion()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    cart.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}, {3, 0, -1, 0, -1, kJump, 0x00}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport bidi = ConvertSong(rom, info, 1, opt, nullptr);
    const agblib::testing::agb_files::MidiEvents midi = ReadMidi(sum.midi_path);
    const agblib::testing::agb_files::Sf2Records sf2 = ReadSf2(sum.sf2_path);
    const agblib::testing::agb_files::Sf2Records bidi_sf2 = ReadSf2(bidi.sf2_path);

    // The plan: a loop of 15 ticks from the start, played twice.
    constexpr int kBeatTicks = 8 * kSpeed;
    const double kTickSeconds = double(kTickSamples) / 16384;
    SEQLIB_CHECK(sum.ok && bidi.ok);
    SEQLIB_CHECK_EQ(sum.tracks, 2);
    SEQLIB_CHECK(std::fabs(sum.loop_start) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 15 * kTickSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 30 * kTickSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.bpm - 60.0 / (kBeatTicks * kTickSeconds)) < 1e-3);
    SEQLIB_CHECK_EQ(midi.division, kBeatTicks);
    const auto metas = midi.Find(0xFF);
    bool tempo = false, loop_start = false, loop_end = false;
    for (const auto& [tick, bytes] : metas)
    {
        tempo = tempo ||
                (bytes[1] == 0x51 && tick == 0 &&
                 ((bytes[3] << 16) | (bytes[4] << 8) | bytes[5]) == int(std::lround(kBeatTicks * kTickSeconds * 1e6)));
        loop_start = loop_start || (bytes[1] == 0x06 && tick == 0);
        loop_end = loop_end || (bytes[1] == 0x06 && tick == 15);
    }

    SEQLIB_CHECK(tempo && loop_start && loop_end);

    // The notes: C-4 on key 60 at the start of each pass, and E-4 on key 64, again 2 rows on from its point.
    const auto first_on = midi.Find(0x90);
    const auto second_on = midi.Find(0x91);
    SEQLIB_REQUIRE_EQ(first_on.size(), 2u);
    SEQLIB_CHECK(first_on[0].tick == 0 && first_on[0].bytes[1] == 60 && first_on[1].tick == 15);
    SEQLIB_REQUIRE_EQ(second_on.size(), 4u);
    SEQLIB_CHECK(second_on[0].bytes[1] == 64 && second_on[1].tick == 6);
    const auto programs = midi.Find(0xC1);
    SEQLIB_CHECK(programs.size() >= 2 && programs[0].bytes[1] != programs[1].bytes[1]);

    // The volume slide: CC11 for volumes 64, 60 and 56 on its channel.
    std::vector<int> expression;
    for (const auto& [tick, bytes] : midi.Find(0xB0))
    {
        if (bytes[1] == cc::kExpression && tick > 0 && tick < 6)
        {
            expression.push_back(bytes[2]);
        }
    }

    int cc10 = 0, cc11_at_60 = 0, cc11_at_56 = 0;
    LevelsToControllers((60 * 64 * 64) >> 13, (60 * 64 * 64) >> 13, cc10, cc11_at_60);
    LevelsToControllers((56 * 64 * 64) >> 13, (56 * 64 * 64) >> 13, cc10, cc11_at_56);
    SEQLIB_CHECK(expression == std::vector<int>({cc11_at_60, cc11_at_56}));

    // The portamento: a bend up on the first channel in row 3.
    bool bent_up = false;
    for (const auto& [tick, bytes] : midi.Find(0xE0))
    {
        bent_up = bent_up || (tick >= 9 && tick < 12 && (bytes[1] | (bytes[2] << 7)) > 8192);
    }

    SEQLIB_CHECK(bent_up);

    // The SoundFont: a preset for each sample and point, and the note from point 256 starting there.
    SEQLIB_CHECK_EQ(sf2.Count("phdr", 38), 4u);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf2));
    bool offset_zone = false;
    for (size_t z = 0; z + 1 < sf2.Count("ibag", 4); z++)
    {
        const auto gens = sf2.ZoneGens(z);
        offset_zone = offset_zone || (gens.count(sf2::kStartAddrsOffset) && gens.at(sf2::kStartAddrsOffset) == 256);
    }

    SEQLIB_CHECK(offset_zone);
    SEQLIB_CHECK_EQ(Le32(sf2.Record("shdr", 46, 0) + 36), kC4Rate);
    SEQLIB_CHECK_EQ(Le32(sf2.Record("shdr", 46, 0) + 28) - Le32(sf2.Record("shdr", 46, 0) + 20),
                    kForwardPoints - kForwardLoop);

    // The sample that loops back and forth plays its loop forwards and then backwards.
    const uint8_t* s = bidi_sf2.Record("shdr", 46, 0);
    SEQLIB_CHECK_EQ(Le32(s + 32) - Le32(s + 28), 2 * kBidiLoop);
    SEQLIB_CHECK_EQ(Le32(s + 32) - Le32(s + 20), kBidiPoints + kBidiLoop);
}

// DumpSong lists the header, the orders and the rows.
void TestDump()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    DriverInfo info;
    std::string error;
    const bool found = DetectDriver(rom, DriverOverrides(), info, error);
    const std::string dump = (agblib::testing::agb_files::TempFolderPath() / "krawall_dump.txt").string();

    const bool dumped = found && DumpSong(rom, info, 0, dump, error);
    const std::vector<uint8_t> dump_bytes = ReadAll(dump);
    const std::string dump_text(dump_bytes.begin(), dump_bytes.end());

    SEQLIB_CHECK(found && info.songs.size() == 1);
    SEQLIB_CHECK(dumped);
    SEQLIB_CHECK(dump_text.find("2 channels, 1 orders, restart at order 0, speed 3, tempo 125") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  0 | c0 C-4 i1 v64 | c1 E-4 i2 pan 20\n") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  4 | c0 jump 00\n") != std::string::npos);
}

// A sample of 20 points that loops forwards over its last 6 gets its loop repeated 6 times: 36 points, the first length
// of kMinLoop points or more.
void TestShortLoop()
{
    Cart cart;
    constexpr uint32_t kHeader = kRomBase + 0xF000;
    cart.Put32(kHeader + kSampleLoopLength, 6);
    cart.Put32(kHeader + kSampleEnd, kHeader + kSamplePoints + 20);
    cart.Put32(kHeader + 8, 8363);
    cart.Put8(kHeader + kSampleLoop, 1);
    for (uint32_t i = 0; i < 20; i++)
    {
        cart.Put8(kHeader + kSamplePoints + i, uint8_t(i * 9));
    }

    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    InstrumentSet set;
    set.Program(kHeader, 0, 0);
    const Sf2File sf = set.Build(rom, info);
    SEQLIB_REQUIRE_EQ(sf.samples.size(), 1);
    SEQLIB_CHECK(agblib::testing::agb_files::LengthenedLoop(sf.samples[0], 14, 6));
}

// A module without instruments plays the game's samples by number: instrument 3 is sample 2, over every note. A
// sample's C-4 plays at 8363 Hz. The lookup adds the sample's relative note to the note's key.
void TestInstruments()
{
    Cart cart;
    cart.Module(1, 4, {{0, 0, 49, 1}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    agblib::krawall::ModuleInfo module;
    SEQLIB_REQUIRE(agblib::krawall::ReadModule(rom, info.build, info.modules[0], module));

    const auto forward = agblib::krawall::ReadSample(rom, info, 0);
    const auto one_shot = agblib::krawall::ReadSample(rom, info, 1);
    const auto instrument = agblib::krawall::ReadInstrument(rom, info, module, 3);
    const auto lookup = agblib::krawall::LookUpInstrument(rom, info, module, 2, 49);
    SEQLIB_REQUIRE(forward && one_shot && instrument && lookup);
    SEQLIB_CHECK(forward->loop == 1 && forward->loop_length == kForwardLoop &&
                 forward->end - forward->data == kForwardPoints);
    SEQLIB_CHECK(forward->rate == kC4Rate && forward->volume == 64 && forward->header == rom.U32(kSampleTable));
    SEQLIB_CHECK(one_shot->loop == 0 && one_shot->volume == 48 && one_shot->end - one_shot->data == kOneShotPoints);
    SEQLIB_CHECK(instrument->address == 0 && instrument->regions == std::vector<agblib::krawall::Region>(
                                                                        {{1, agblib::krawall::kInstrumentNotes, 2}}));
    SEQLIB_CHECK(lookup->instrument == 0 && lookup->sample == 1 && lookup->header == rom.U32(kSampleTable + 4) &&
                 lookup->key == 48);
    SEQLIB_CHECK(!agblib::krawall::LookUpInstrument(rom, info, module, 2, agblib::krawall::kNoteOff));
    SEQLIB_CHECK(!agblib::krawall::ReadInstrument(rom, info, module, 0));
}

// An audition plays a note of an instrument on the module's first channel. The forward loop holds the note, and the
// rows don't play. The release stops the note: without an envelope, a note off stops the channel.
void TestAudition()
{
    Cart cart;
    cart.Module(1, 4, {{0, 0, 49, 2}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    agblib::krawall::ArchivePlayer player(rom, info);
    player.StartNote(0, 1, 61);
    std::vector<int16_t> out;
    for (int f = 0; f < 30; f++)
    {
        player.RunFrame(out);
    }

    const bool sounds = std::any_of(out.end() - 1000, out.end(), [&out](int16_t v) { return v != out.back(); });
    const bool busy = player.Busy();
    const auto& mixer = player.Driver().MixerOf(player.Driver().Channels()[0]);
    const bool forward = mixer.sample == rom.U32(kSampleTable) && player.Driver().RowNumber() == 0;
    player.ReleaseNote();
    int frames = 0;
    for (; frames < 20 && player.Busy(); frames++)
    {
        player.RunFrame(out);
    }

    std::vector<int16_t> after;
    player.RunFrame(after);
    const bool silent = std::all_of(after.begin(), after.end(), [&after](int16_t v) { return v == after.front(); });

    SEQLIB_CHECK(sounds && busy && forward);
    SEQLIB_CHECK(frames > 0 && frames < 20);
    SEQLIB_CHECK(!after.empty() && silent && player.Driver().MixerOf(player.Driver().Channels()[0]).status == 0);
}

// ConvertSongFiles gives the bytes of the files that ConvertSong writes, and names no paths.
void TestSongFiles()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_files";

    const SongReport written = ConvertSong(rom, info, 0, opt, nullptr);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);

    CheckFilesInMemory(written, files);
}

// The performance holds the notes of the MIDI file, each with its row's entry. The module plays its 5 rows, and the
// jump on row 4 goes back to row 0. Each row is a stretch, every 3 ticks: the row that the jump goes back to has
// Entry::kLoop, and the others Entry::kOpen. Channel 0's notes start at its entry on row 0, and channel 1's at its
// entries on rows 0 and 2. Channel 0's volume starts at 64, and the slide on row 1 takes it to 60 and 56 on the row's
// second and third ticks. Each tick is a MIDI tick.
void TestPerformance()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.loops = 2;

    const Performance p = PerformSong(rom, info, 0, opt);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);
    const agblib::krawall::Decoded decoded = agblib::krawall::DecodeRomSequence(rom, info, 0);
    const std::vector<uint32_t> offsets =
        agblib::krawall::ItemOffsets(decoded.sequence, agblib::krawall::TraitsOf(info));

    std::vector<std::tuple<uint32_t, int, uint32_t>> notes;
    std::array<std::vector<uint32_t>, 2> entries;
    std::map<uint32_t, uint32_t> ons;
    for (size_t t = 0; t < 2; t++)
    {
        for (const ntdlib::sequence_export::TrackEvent& e : p.tracks[t])
        {
            if (e.kind == ntdlib::sequence_export::EventKind::kNoteOn)
            {
                ons[e.note] = e.tick;
                entries[t].push_back(p.note_commands[e.note - 1].offset);
            }
            else if (e.kind == ntdlib::sequence_export::EventKind::kNoteOff)
            {
                notes.emplace_back(ons[e.note], e.key, e.tick);
            }
        }
    }

    std::vector<std::tuple<uint32_t, int, uint32_t>> midi;
    for (const seqlib::midi::Note& n : seqlib::midi::Notes(seqlib::midi::Read(files.midi)))
    {
        midi.emplace_back(uint32_t(n.on), int(n.key), uint32_t(n.off));
    }

    std::sort(notes.begin(), notes.end());
    std::sort(midi.begin(), midi.end());

    const std::vector<ntdlib::sequence_export::Stretch>& stretches = p.stretches[0];
    const std::vector<ntdlib::sequence_export::MixChange>& mix = p.mix[0];
    const uint32_t row_0 = offsets[1];
    const uint32_t end = offsets[decoded.sequence.items.size() - 1];
    SEQLIB_REQUIRE(entries[0].size() >= 2 && entries[1].size() >= 4 && stretches.size() >= 6 && mix.size() >= 4);
    SEQLIB_CHECK(p.base == agblib::krawall::SequenceAddress(rom, info, 0) && p.loop && p.loops == 2);
    SEQLIB_CHECK(notes == midi);
    SEQLIB_CHECK(entries[0][0] == offsets[1] && entries[0][1] == offsets[1]);
    SEQLIB_CHECK(entries[1][0] == offsets[3] && entries[1][1] != offsets[3] && entries[1][2] == offsets[3]);
    for (size_t i = 0; i < 5; i++)
    {
        SEQLIB_CHECK(stretches[i].tick == 3 * i && stretches[i].entry == ntdlib::sequence_export::Entry::kOpen &&
                     stretches[i].first <= stretches[i].last);
        SEQLIB_CHECK(i == 0 || stretches[i].first > stretches[i - 1].last);
    }

    SEQLIB_CHECK(stretches[0].first == row_0 && stretches[4].last == end);
    SEQLIB_CHECK(stretches[5] == (ntdlib::sequence_export::Stretch{15, row_0, stretches[0].last,
                                                                   ntdlib::sequence_export::Entry::kLoop, 0, false}));
    SEQLIB_CHECK(p.note_commands[0].stretch == 0 && p.stretches[1] == stretches && p.stretches[2].empty());
    SEQLIB_CHECK(mix[0] == (ntdlib::sequence_export::MixChange{0, ntdlib::sequence_export::Mix::kVolume, 64}) &&
                 mix[1] == (ntdlib::sequence_export::MixChange{0, ntdlib::sequence_export::Mix::kPan, 0}));
    SEQLIB_CHECK(mix[2] == (ntdlib::sequence_export::MixChange{4, ntdlib::sequence_export::Mix::kVolume, 60}) &&
                 mix[3] == (ntdlib::sequence_export::MixChange{5, ntdlib::sequence_export::Mix::kVolume, 56}));
}

// Each stretch keeps the contract of a stretch: its entries play at its tick, and the wait at its end gives the next
// row's tick. Speed effects set speed 3 on row 0 and speed 6 on row 2, and the jump on row 5 goes back to row 0. Each
// row is a stretch. The rows at speed 6 last twice as many MIDI ticks as the rows at speed 3, and every note starts at
// its row's tick.
void TestPerformanceAtTwoSpeeds()
{
    Cart cart;
    cart.Module(1, 6,
                {{0, 0, 49, 1, -1, kSetSpeed, 3},
                 {1, 0, 51, 1, -1, -1, 0},
                 {2, 0, 53, 1, -1, kSetSpeed, 6},
                 {3, 0, 55, 1, -1, -1, 0},
                 {4, 0, 56, 1, -1, -1, 0},
                 {5, 0, 58, 1, -1, kJump, 0x00}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.loops = 2;

    const Performance p = PerformSong(rom, info, 0, opt);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);

    const std::vector<ntdlib::sequence_export::Stretch>& stretches = p.stretches[0];
    SEQLIB_REQUIRE(stretches.size() >= 12);
    int notes = 0;
    for (const ntdlib::sequence_export::TrackEvent& e : p.tracks[0])
    {
        if (e.kind == ntdlib::sequence_export::EventKind::kNoteOn)
        {
            const ntdlib::sequence_export::NoteCommand& command = p.note_commands[e.note - 1];
            SEQLIB_REQUIRE(command.stretch < stretches.size());
            const ntdlib::sequence_export::Stretch& s = stretches[command.stretch];
            SEQLIB_CHECK_EQ(s.tick, e.tick);
            SEQLIB_CHECK(s.first <= command.offset && command.offset <= s.last);
            notes++;
        }
    }

    const size_t midi_notes = seqlib::midi::Notes(seqlib::midi::Read(files.midi)).size();

    const uint32_t fast = stretches[1].tick - stretches[0].tick;
    const uint32_t slow = stretches[3].tick - stretches[2].tick;
    SEQLIB_CHECK(notes >= 12 && size_t(notes) == midi_notes);
    SEQLIB_CHECK(fast > 0 && slow == 2 * fast && stretches[2].tick - stretches[1].tick == fast);
    SEQLIB_CHECK(stretches[4].tick - stretches[3].tick == slow && stretches[5].tick - stretches[4].tick == slow);
    SEQLIB_CHECK(stretches[6].tick - stretches[5].tick == slow && stretches[7].tick - stretches[6].tick == fast);
    for (size_t i = 0; i < 12; i++)
    {
        SEQLIB_CHECK(stretches[i].entry ==
                         (i == 6 ? ntdlib::sequence_export::Entry::kLoop : ntdlib::sequence_export::Entry::kOpen) &&
                     stretches[i].depth == 0);
        SEQLIB_CHECK(stretches[i].first <= stretches[i].last);
    }
}

} // namespace

int RunTests()
{
    TestDetection();
    TestPlayer();
    TestMixer();
    TestEnhancedAttack();
    TestSongs();
    TestConversion();
    TestDump();
    TestShortLoop();
    TestInstruments();
    TestAudition();
    TestSongFiles();
    TestPerformance();
    TestPerformanceAtTwoSpeeds();

    return seqlib::testing::Summary("krawall");
}

} // namespace agblib::sequence_export::krawall

int main()
{
    return agblib::sequence_export::krawall::RunTests();
}
