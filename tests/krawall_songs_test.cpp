// SPDX-License-Identifier: MIT

// Unit tests of the songs of Krawall's games: jingles, the games' tables of songs, and the settings of modules,
// songs and games. They build small synthetic cartridges in memory. No game ROM is needed.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

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

// A whole module that the game plays once, through its routine for jingles, converts once and without loop markers. In
// loop mode, the same module plays its pattern twice. The log names the routine and the jingle. A call of the routine
// whose entry detection can't read gets a warning.
void TestJingles()
{
    Cart cart;
    const uint32_t looped = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t jingle = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.Jingle(1);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_jingles";

    const SongReport loop = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport once = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{looped, -1, false}, {jingle, -1, true}}));
    const auto logged = [&info](const std::string& line)
    {
        return std::find(info.log.begin(), info.log.end(), line) != info.log.end();
    };
    SEQLIB_CHECK(logged("the game's routine that plays a module once, as a jingle: 0x08000300"));
    SEQLIB_CHECK(logged("jingles, which play once: 1"));
    SEQLIB_CHECK(info.warnings == std::vector<std::string>({"the game plays a module once from 0x08000348, by a number "
                                                            "that detection can't read. That module converts as a "
                                                            "loop"}));
    SEQLIB_REQUIRE(loop.ok && once.ok);
    SEQLIB_CHECK(loop.loop_end > 0 && once.loop_start < 0 && once.loop_end < 0);
    SEQLIB_CHECK_EQ(ReadMidi(loop.midi_path).Find(0x90).size(), 2u);
    SEQLIB_CHECK_EQ(ReadMidi(once.midi_path).Find(0x90).size(), 1u);
    bool marker = false;
    for (const auto& [tick, bytes] : ReadMidi(once.midi_path).Find(0xFF))
    {
        marker = marker || bytes[1] == 0x06;
    }

    SEQLIB_CHECK(!marker);
}

// In the jingle mode with the loop bit, krapPlay() loops the module. An entry that a routine plays in that mode isn't
// a jingle. The entry's module converts as a loop, without a warning for the call that passes r5.
void TestLoopingJingleMode()
{
    Cart cart;
    const uint32_t module = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.Jingle(0, kModeJingle | kModeLoop);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{module, -1, false}}));
    SEQLIB_CHECK(info.warnings.empty());
    SEQLIB_CHECK(std::none_of(info.log.begin(), info.log.end(),
                              [](const std::string& line) { return line.find("jingle") != std::string::npos; }));
}

// The entries of the game's table of what it plays come first, each in its mode, without the entry that repeats one.
// The module that no entry plays follows. The entry in song mode without the loop bit plays its module once and
// converts without loop markers. The log names the table and the entry whose mode doesn't loop.
void TestPlayTable()
{
    Cart cart;
    const uint32_t looped = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t once = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t unplayed = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.PlayTable({{once, kModeSong, 0}, {looped, kModeLoop, 0}, {once, kModeSong, 0}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_play_table";

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(
        info.songs ==
        (std::vector<ModuleSong>{{once, -1, false, kModeSong}, {looped, -1, false, kModeLoop}, {unplayed, -1, false}}));
    const auto logged = [&info](const std::string& line)
    {
        return std::find(info.log.begin(), info.log.end(), line) != info.log.end();
    };
    SEQLIB_CHECK(logged("the game's table of what it plays: 0x08002800 (3 entries)"));
    SEQLIB_CHECK(logged("songs whose mode doesn't loop: 0"));
    SEQLIB_REQUIRE(first.ok && second.ok);
    SEQLIB_CHECK(first.loop_start < 0 && first.loop_end < 0 && second.loop_end > 0);
    SEQLIB_CHECK_EQ(ReadMidi(first.midi_path).Find(0x90).size(), 1u);
}

// krapPlay()'s mode 0 plays a module once. An entry in that mode doesn't end the game's table of what it plays.
void TestPlayTableModeZero()
{
    Cart cart;
    const uint32_t once = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t looped = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.PlayTable({{once, 0, 0}, {looped, kModeLoop, 0}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_mode_zero";

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{once, -1, false, 0}, {looped, -1, false, kModeLoop}}));
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(), "songs whose mode doesn't loop: 0") != info.log.end());
    SEQLIB_REQUIRE(first.ok && second.ok);
    SEQLIB_CHECK(first.loop_start < 0 && first.loop_end < 0 && second.loop_end > 0);
    SEQLIB_CHECK_EQ(ReadMidi(first.midi_path).Find(0x90).size(), 1u);
}

// Franklin's Great Adventures plays each entry of its table of modules in the mode and at the music volume of its
// tables. Its start-up code copies the table of modules to RAM. The entry that repeats one is left out. The player
// sets the music volume after it starts the module, as krapSetMusicVol() does.
void TestModuleSettings()
{
    Cart cart;
    const uint32_t looped = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t once = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.Put32(kModuleTable + 8, looped);
    cart.ModuleSettings(0x03006050, {kModeLoop, 0, kModeLoop}, {90, 70, 90});
    Cart bad;
    bad.Module(1, 4, {{0, 0, 49, 1}});
    bad.ModuleSettings(kModuleTable, {8}, {90});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const DriverInfo bad_info = Detect(bad.ToRom());
    SEQLIB_REQUIRE(info.songs.size() == 2);
    const Player player(rom, info, info.songs[0]);
    const Player full(rom, info, looped);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{looped, -1, false, kModeLoop, 90}, {once, -1, false, 0, 70}}));
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(),
                           "the game's routine at 0x08000380 plays each module in the mode of the table at "
                           "0x08002800 and at the music volume of the table at 0x08002900") != info.log.end());
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(), "songs whose mode doesn't loop: 1") != info.log.end());
    SEQLIB_CHECK_EQ(player.Globals()[8], 90);
    SEQLIB_CHECK_EQ(full.Globals()[8], 128);
    SEQLIB_CHECK_EQ(player.Globals()[0] * 128, full.Globals()[0] * 90);
    SEQLIB_CHECK(bad_info.songs == (std::vector<ModuleSong>{{bad_info.modules[0], -1, false}}));
}

// Care Bears: Care Quest plays song 1 of its module at 0x08360B0C at a music volume of 64, and song 2 once, in the song
// mode. Franklin the Turtle plays its module at 0x08335F60 only as a jingle. The Koala Brothers plays its music at 64,
// The Lord of the Rings: The Third Age at 153, Crash of the Titans at 96 and Digimon Racing at 180. A piece that no
// row names keeps the mode that the song and jingle give and the game's music volume.
void TestSongSettings()
{
    Cart bears;
    bears.SetGameCode("BEAE");
    const uint32_t module =
        bears.Module(1, {{4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}}, true, 0x08360B0C);
    bears.Songs(module, {{0}, {1}, {2}});
    Cart franklin;
    franklin.SetGameCode("BFKE");
    const uint32_t piece = franklin.Module(1, 4, {{0, 0, 49, 1}}, true, 0x08334368);
    const uint32_t jingle = franklin.Module(1, 4, {{0, 0, 49, 1}}, true, 0x08335F60);
    const uint32_t other = franklin.Module(1, 4, {{0, 0, 49, 1}});
    Cart koalas;
    koalas.SetGameCode("BAKE");
    koalas.Module(1, 4, {{0, 0, 49, 1}});
    Cart third_age;
    third_age.SetGameCode("B3AE");
    third_age.Module(1, 4, {{0, 0, 49, 1}});
    Cart crash;
    crash.SetGameCode("BQCE");
    crash.Module(1, 4, {{0, 0, 49, 1}});
    Cart digimon;
    digimon.SetGameCode("BDGE");
    digimon.Module(1, 4, {{0, 0, 49, 1}});
    Cart digimon_japan;
    digimon_japan.SetGameCode("BDJJ");
    digimon_japan.Module(1, 4, {{0, 0, 49, 1}});
    const Rom rom = bears.ToRom();
    const DriverInfo info = Detect(rom);
    const DriverInfo franklin_info = Detect(franklin.ToRom());
    SEQLIB_REQUIRE(info.songs.size() == 3);
    const Player first(rom, info, info.songs[0]);
    const Player second(rom, info, info.songs[1]);
    ConvertSettings opt;
    opt.out_dir = agblib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_song_settings";

    const SongReport once = ConvertSong(rom, info, 2, opt, nullptr);

    SEQLIB_CHECK(info.songs ==
                 (std::vector<ModuleSong>{{module, 0}, {module, 1, false, -1, 64}, {module, 2, false, kModeSong}}));
    const auto logged = [&info](const std::string& line)
    {
        return std::find(info.log.begin(), info.log.end(), line) != info.log.end();
    };
    SEQLIB_CHECK(logged("the game plays song 1 at a music volume of 64"));
    SEQLIB_CHECK(logged("the game plays song 2 in mode 2"));
    SEQLIB_CHECK_EQ(first.Globals()[8], 128);
    SEQLIB_CHECK_EQ(second.Globals()[8], 64);
    SEQLIB_CHECK_EQ(second.Globals()[0] * 128, first.Globals()[0] * 64);
    SEQLIB_REQUIRE(once.ok);
    SEQLIB_CHECK(once.loop_start < 0 && once.loop_end < 0);
    SEQLIB_CHECK(
        franklin_info.songs ==
        (std::vector<ModuleSong>{{piece, -1, false, -1, 64}, {jingle, -1, false, kModeJingle}, {other, -1, false}}));
    SEQLIB_CHECK_EQ(Detect(koalas.ToRom()).music_volume, 64);
    SEQLIB_CHECK_EQ(Detect(third_age.ToRom()).music_volume, 153);
    SEQLIB_CHECK_EQ(Detect(crash.ToRom()).music_volume, 96);
    SEQLIB_CHECK_EQ(Detect(digimon.ToRom()).music_volume, 180);
    SEQLIB_CHECK_EQ(Detect(digimon_japan.ToRom()).music_volume, 180);
}

// The other releases of the games with rows: their music volumes and mixers, and the songs of the European Care Bears
// and of each revision of the European Franklin.
void TestSongSettingsOfReleases()
{
    const auto release = [](const char* game_code)
    {
        Cart cart;
        cart.SetGameCode(game_code);
        cart.Module(1, 4, {{0, 0, 49, 1}});
        return Detect(cart.ToRom());
    };
    SEQLIB_CHECK_EQ(release("BENP").music_volume, 161);
    SEQLIB_CHECK_EQ(release("BHTJ").music_volume, 120);
    SEQLIB_CHECK_EQ(release("BJKP").music_volume, 128);
    SEQLIB_CHECK_EQ(release("BJKP").music_fade_in, 2);
    SEQLIB_CHECK_EQ(release("BAKP").music_volume, 64);
    SEQLIB_CHECK_EQ(release("B3AJ").music_volume, 153);
    SEQLIB_CHECK_EQ(release("BQCP").music_volume, 96);
    SEQLIB_CHECK_EQ(release("BDGP").music_volume, 180);
    for (const char* game_code : {"B63P", "BCZE"})
    {
        const DriverInfo modified = release(game_code);
        SEQLIB_CHECK(modified.stereo);
        SEQLIB_CHECK_EQ(modified.quality, 0x21);
    }

    SEQLIB_CHECK_EQ(release("B63P").music_volume, 100);
    SEQLIB_CHECK_EQ(release("BCZE").music_volume, 64);
    const std::pair<const char*, uint32_t> kEurope[] = {
        {"B4MP", 64}, {"BBAP", 60}, {"BKFX", 64}, {"BU5P", 80}, {"BU7P", 85}, {"BUIP", 48}, {"BUQP", 80},
    };
    for (const auto& [code, volume] : kEurope)
    {
        SEQLIB_CHECK_EQ(release(code).music_volume, volume);
    }

    SEQLIB_CHECK_EQ(int(release("BU7P").quality), 1);

    Cart bears_europe;
    bears_europe.SetGameCode("BEAP");
    const uint32_t bears_module = bears_europe.Module(
        1, {{4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}}, true, 0x08361B28);
    bears_europe.Songs(bears_module, {{0}, {1}, {2}, {3}});
    SEQLIB_CHECK(Detect(bears_europe.ToRom()).songs == (std::vector<ModuleSong>{{bears_module, 0},
                                                                                {bears_module, 1, false, -1, 64},
                                                                                {bears_module, 2},
                                                                                {bears_module, 3, false, kModeSong}}));
    for (const uint32_t revision_jingle : {0x08331178u, 0x08336D64u, 0x08335F60u})
    {
        Cart franklin_europe;
        franklin_europe.SetGameCode("BFKP");
        const uint32_t jingle_europe = franklin_europe.Module(1, 4, {{0, 0, 49, 1}}, true, revision_jingle);
        SEQLIB_CHECK(Detect(franklin_europe.ToRom()).songs ==
                     (std::vector<ModuleSong>{{jingle_europe, -1, false, kModeJingle}}));
    }

    for (const uint32_t revision_piece : {0x0832F580u, 0x0833516Cu, 0x08334368u})
    {
        Cart franklin_europe;
        franklin_europe.SetGameCode("BFKP");
        const uint32_t piece_europe = franklin_europe.Module(1, 4, {{0, 0, 49, 1}}, true, revision_piece);
        SEQLIB_CHECK(Detect(franklin_europe.ToRom()).songs ==
                     (std::vector<ModuleSong>{{piece_europe, -1, false, -1, 64}}));
    }
}

// The music volumes and mixers that the games' code sets, The Bee Game's song mode and Postman Pat's table of music
// volumes.
void TestGameCodeSettings()
{
    const std::pair<const char*, uint32_t> volumes[] = {
        {"B2WE", 254}, {"B3UE", 96}, {"B46E", 180}, {"B4ME", 64}, {"B8QE", 140}, {"BBAE", 60}, {"BC4E", 96},
        {"BJXE", 80},  {"BKFE", 64}, {"BU5E", 80},  {"BU7E", 85}, {"BUIE", 48},  {"BUQE", 80}, {"BXPE", 77},
    };
    for (const auto& [code, volume] : volumes)
    {
        Cart game;
        game.SetGameCode(code);
        game.Module(1, 4, {{0, 0, 49, 1}});
        SEQLIB_CHECK_EQ(Detect(game.ToRom()).music_volume, volume);
    }

    Cart spyro;
    spyro.SetGameCode("BU7E");
    spyro.Module(1, 4, {{0, 0, 49, 1}});
    SEQLIB_CHECK_EQ(int(Detect(spyro.ToRom()).quality), 1);

    // The Bee Game plays its modules in the song mode.
    Cart bee;
    bee.SetGameCode("BKFE");
    const uint32_t bee_module = bee.Module(1, 4, {{0, 0, 49, 1}}, true, 0x08380B3C);
    SEQLIB_CHECK(Detect(bee.ToRom()).songs == (std::vector<ModuleSong>{{bee_module, -1, false, kModeSong}}));
    Cart bee_europe;
    bee_europe.SetGameCode("BKFX");
    const uint32_t bee_europe_module = bee_europe.Module(1, 4, {{0, 0, 49, 1}}, true, 0x0837B0D4);
    SEQLIB_CHECK(Detect(bee_europe.ToRom()).songs ==
                 (std::vector<ModuleSong>{{bee_europe_module, -1, false, kModeSong}}));

    // Postman Pat's table gives each entry of the game's table of modules a music volume.
    Cart pat;
    pat.SetGameCode("BROP");
    const uint32_t pat_first = pat.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t pat_second = pat.Module(1, 4, {{0, 0, 49, 1}});
    pat.Resize(0x40000);
    pat.Put8(0x08032C48, 128);
    pat.Put8(0x08032C49, 90);
    SEQLIB_CHECK(Detect(pat.ToRom()).songs ==
                 (std::vector<ModuleSong>{{pat_first, -1, false, -1, 128}, {pat_second, -1, false, -1, 90}}));
}

// Cocoto Kart Racer plays its module at 0x0826181C at a music volume of 100. Cocoto Platform Jumper plays its module at
// 0x0802DE90 at 73. A piece that no row names keeps the game's music volume, and a row counts only for its game.
void TestCocotoSongSettings()
{
    Cart kart;
    kart.SetGameCode("BC5P");
    const uint32_t race = kart.Module(1, 4, {{0, 0, 49, 1}}, true, 0x0826181C);
    const uint32_t menu = kart.Module(1, 4, {{0, 0, 49, 1}});
    Cart jumper;
    jumper.SetGameCode("BC8P");
    const uint32_t level = jumper.Module(1, 4, {{0, 0, 49, 1}}, true, 0x0802DE90);
    Cart other;
    other.SetGameCode("ZZZZ");
    const uint32_t same_place = other.Module(1, 4, {{0, 0, 49, 1}}, true, 0x0802DE90);
    const Rom rom = kart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_REQUIRE(info.songs.size() == 2);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{race, -1, false, -1, 100}, {menu, -1, false}}));
    SEQLIB_CHECK(Detect(jumper.ToRom()).songs == (std::vector<ModuleSong>{{level, -1, false, -1, 73}}));
    SEQLIB_CHECK(Detect(other.ToRom()).songs == (std::vector<ModuleSong>{{same_place, -1, false}}));
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(), "the game plays song 0 at a music volume of 100") !=
                 info.log.end());
    const Player raced(rom, info, info.songs[0]);
    const Player unlisted(rom, info, info.songs[1]);
    SEQLIB_CHECK_EQ(raced.Globals()[8], 100);
    SEQLIB_CHECK_EQ(unlisted.Globals()[8], 128);
    SEQLIB_CHECK_EQ(raced.Globals()[0] * 128, unlisted.Globals()[0] * 100);
}

} // namespace

int RunTests()
{
    TestJingles();
    TestLoopingJingleMode();
    TestPlayTable();
    TestPlayTableModeZero();
    TestModuleSettings();
    TestSongSettings();
    TestSongSettingsOfReleases();
    TestGameCodeSettings();
    TestCocotoSongSettings();

    return seqlib::testing::Summary("krawall_songs");
}

} // namespace agblib::sequence_export::krawall

int main()
{
    return agblib::sequence_export::krawall::RunTests();
}
