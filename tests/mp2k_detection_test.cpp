// SPDX-License-Identifier: MIT

// Unit tests for MP2K's reader: the decoding of a track's commands, the detection of the driver and its sound modes,
// and the players that a game leaves unused. They build small synthetic cartridges in memory. No game ROM is needed.

#include <string>
#include <vector>

#include "mp2k_test_util.h"

namespace agblib::sequence_export::mp2k
{
namespace
{

void TestDecoding()
{
    Cart cart;
    Track t;
    const size_t start = t.Here();
    t.Cmd(kCmdKeySh, 0)
        .Cmd(kCmdVoice, 5)
        .Raw({0x30})
        .Note(8, 60, 100, 2)
        .Raw({0x3E})
        .Wait(24)
        .Tie(64, 127)
        .Eot(64)
        .Raw({kCmdXcmd, kXcmdEchoVol, 16})
        .Raw({kCmdXcmd, kXcmdWave, 0x56, 0x34, 0x12, 0x08})
        .Raw({kCmdXcmd, kXcmdOffset, 0x40, 0x01, 0x00, 0x00})
        .Patt(start)
        .Rept(2, start)
        .Raw({kCmdMemAcc, 6, 1, 2, 0, 0, 0, 0})
        .Goto(start)
        .Fine();
    const uint32_t at = cart.Place(t.Bytes(kData), 1);
    const Rom rom = cart.ToRom();

    uint32_t a = at;
    uint8_t running = 0;
    Event e;
    auto next = [&]()
    {
        const bool ok = DecodeEvent(rom, a, running, e);
        running = RunningStatus(e, running);
        a += e.size;

        return ok;
    };

    SEQLIB_CHECK(next() && e.command == kCmdKeySh && e.size == 2 && DescribeEvent(e) == "KEYSH 0");
    SEQLIB_CHECK(next() && e.command == kCmdVoice && e.arg[0] == 5);
    SEQLIB_CHECK(next() && e.command == kCmdVoice && e.size == 1 && e.arg[0] == 0x30);
    SEQLIB_CHECK(next() && e.size == 4 && DescribeEvent(e) == "N08 key 60 vel 100 gate+2");
    SEQLIB_CHECK(next() && e.size == 1 && DescribeEvent(e) == "N08 key 62");
    SEQLIB_CHECK(next() && DescribeEvent(e) == "W24");
    SEQLIB_CHECK(next() && DescribeEvent(e) == "TIE key 64 vel 127");
    SEQLIB_CHECK(next() && DescribeEvent(e) == "EOT key 64");
    SEQLIB_CHECK(next() && e.size == 3 && DescribeEvent(e) == "XCMD xIECV 16");
    SEQLIB_CHECK(next() && e.size == 6 && e.target == 0x08123456 && DescribeEvent(e) == "XCMD xWAVE 0x08123456");
    SEQLIB_CHECK(next() && e.size == 6 && e.target == 320 && DescribeEvent(e) == "XCMD x0D 320");
    SEQLIB_CHECK(next() && e.command == kCmdPatt && e.target == at);
    SEQLIB_CHECK(next() && e.command == kCmdRept && e.size == 6 && e.arg[0] == 2 && e.target == at);
    SEQLIB_CHECK(next() && e.command == kCmdMemAcc && e.size == 8 && e.target == 0);
    SEQLIB_CHECK(next() && e.command == kCmdGoto && e.target == at);
    SEQLIB_CHECK(next() && e.command == kCmdFine);

    // A byte below 0x80 without a running status, and an extended command the driver doesn't have.
    const uint32_t bad = cart.Place({0x40, kCmdXcmd, 0x20}, 1);
    const Rom bad_rom = cart.ToRom();

    SEQLIB_CHECK(!DecodeEvent(bad_rom, bad, 0, e));
    SEQLIB_CHECK(!DecodeEvent(bad_rom, bad + 1, 0, e));
}

// The address that a new cart gives its first voice group, of one saw. Carts without a voice group of their own use it
// for their songs.
uint32_t SawVoices()
{
    Cart cart;
    return cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}});
}

void TestDetection()
{
    // The routine that starts a song gives the tables, and the init gives the mixer's settings. The song table ends
    // before the first entry that isn't a song.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}});
    cart.Song({Track().Fine()}, voices);
    cart.Song({Track().Fine(), Track().Fine()}, voices, 1);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK_EQ(info.song_start, kSongStart);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.player_table, kPlayerTable);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_REQUIRE_EQ(info.players.size(), 2);
    SEQLIB_CHECK(info.players[1].track_count == 3);
    SEQLIB_CHECK_EQ(info.sound_mode, kMode);
    SEQLIB_CHECK(info.game_modes.empty());
    SEQLIB_CHECK_EQ(info.max_channels, 5);
    SEQLIB_CHECK_EQ(info.master_volume, 12);
    SEQLIB_CHECK_EQ(info.mix_rate, 13379);
    SEQLIB_CHECK_EQ(info.samples_per_frame, 224);
    SEQLIB_CHECK_EQ(info.step_scale, 627);
    SEQLIB_CHECK(info.special_samples);
    SEQLIB_CHECK(!info.mono);
    SEQLIB_CHECK(info.warnings.empty());

    // Overrides take the place of what detection finds.
    DriverOverrides overrides;
    overrides.song_count = 1;
    DriverInfo one;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, overrides, one, error));
    SEQLIB_CHECK_EQ(one.song_count, 1);

    // An all-zero entry is empty and doesn't end the song table. A GSF rip can leave one for a song that it doesn't
    // contain. The all-zero entries after the last song don't count. A table without a song fails, with a hint.
    Cart gaps;
    for (int s = 0; s < 4; s++)
    {
        gaps.Song({Track().Fine()}, voices);
    }

    for (const uint32_t entry : {kSongTable + 8, kSongTable + 24})
    {
        gaps.Put32(entry, 0);
        gaps.Put32(entry + 4, 0);
    }

    const Rom gaps_rom = gaps.ToRom();
    const Rom empty_rom = Cart().ToRom();
    DriverInfo empty;

    const DriverInfo gaps_info = Detect(gaps_rom);
    const bool found_empty = DetectDriver(empty_rom, DriverOverrides(), empty, error);

    SEQLIB_CHECK_EQ(gaps_info.song_count, 3);
    SEQLIB_CHECK_EQ(SongAddress(gaps_rom, gaps_info, 1), 0);
    SEQLIB_CHECK(SongAddress(gaps_rom, gaps_info, 0) != 0 && SongAddress(gaps_rom, gaps_info, 2) != 0);
    SEQLIB_CHECK(!Sequencer(gaps_rom, gaps_info, 1).Valid() && Sequencer(gaps_rom, gaps_info, 2).Valid());
    SEQLIB_CHECK(!found_empty);
    SEQLIB_CHECK(error.find("--song-count") != std::string::npos);

    // Another mode, an older mixer, and no mode at all: then the driver's defaults stand.
    Cart slow(0x0093F800, {4}, true, false);
    slow.Song({Track().Fine()}, voices);
    Cart unset(0x00000500);
    unset.Song({Track().Fine()}, voices);

    const DriverInfo slow_info = Detect(slow.ToRom());
    const DriverInfo unset_info = Detect(unset.ToRom());

    SEQLIB_CHECK_EQ(slow_info.mix_rate, 10512);
    SEQLIB_CHECK_EQ(slow_info.max_channels, 8);
    SEQLIB_CHECK(!slow_info.special_samples);
    SEQLIB_CHECK_EQ(unset_info.sound_mode, 0);
    SEQLIB_CHECK_EQ(unset_info.max_channels, 8);
    SEQLIB_CHECK_EQ(unset_info.mix_rate, 13379);
    SEQLIB_CHECK(!unset_info.warnings.empty());
}

// The sound modes that a game's code sets: Metroid Fusion's mode kept in r8, a mode set again after the init, and the
// row of a game that works its mode out while it runs.
void TestSoundModes()
{
    const uint32_t voices = SawVoices();

    // Metroid Fusion's init keeps its mode in r8 for a later call, and the mode's top byte of 1 asks for stereo.
    // Without the move, nothing marks the load as the mode's.
    Cart fusion(0x0193F600, {4});
    fusion.Put16(kInit + 2, 0x4680);
    fusion.Put16(kInit + 4, 0);
    fusion.Song({Track().Fine()}, voices);
    Cart loose(0x0193F600, {4});
    loose.Put16(kInit + 2, 0);
    loose.Put16(kInit + 4, 0);
    loose.Song({Track().Fine()}, voices);

    const DriverInfo fusion_info = Detect(fusion.ToRom());
    const DriverInfo loose_info = Detect(loose.ToRom());

    SEQLIB_CHECK_EQ(fusion_info.sound_mode, 0x0193F600);
    SEQLIB_CHECK_EQ(fusion_info.mix_rate, 10512);
    SEQLIB_CHECK_EQ(fusion_info.max_channels, 6);
    SEQLIB_CHECK_EQ(fusion_info.master_volume, 15);
    SEQLIB_CHECK_EQ(loose_info.sound_mode, 0);

    // A game's code can set the mode again straight after the init: bl m4aSoundInit; ldr r0, =mode; bl m4aSoundMode.
    // Each field of that mode that isn't 0 replaces the init's. A mode that the code sets elsewhere doesn't count.
    Cart again;
    again.GameSoundMode(0x00960680, true);
    again.Song({Track().Fine()}, voices);
    const DriverInfo again_info = Detect(again.ToRom());
    SEQLIB_CHECK_EQ(again_info.sound_mode, kMode);
    SEQLIB_CHECK(again_info.game_modes == std::vector<uint32_t>{0x00960680});
    SEQLIB_CHECK_EQ(again_info.max_channels, 6);
    SEQLIB_CHECK_EQ(again_info.master_volume, 12);
    SEQLIB_CHECK_EQ(again_info.mix_rate, 18157);
    Cart elsewhere;
    elsewhere.GameSoundMode(0x00960680, false);
    elsewhere.Song({Track().Fine()}, voices);
    const DriverInfo elsewhere_info = Detect(elsewhere.ToRom());
    SEQLIB_CHECK_EQ(elsewhere_info.max_channels, 5);
    SEQLIB_CHECK(elsewhere_info.game_modes.empty());

    // A row gives the mode of a game that works it out while it runs.
    Cart spyro;
    spyro.SetGameCode("ASYE");
    spyro.Song({Track().Fine()}, voices);
    const DriverInfo spyro_info = Detect(spyro.ToRom());
    SEQLIB_CHECK(spyro_info.game_modes == std::vector<uint32_t>{0x00D4A880});
    SEQLIB_CHECK_EQ(spyro_info.max_channels, 8);
    SEQLIB_CHECK_EQ(spyro_info.master_volume, 10);
    SEQLIB_CHECK_EQ(spyro_info.mix_rate, 13379);
}

// The sound modes that a game's code shifts into place before each call, and the row of a game that sets its mode each
// time it starts a song.
void TestShiftedSoundModes()
{
    const uint32_t voices = SawVoices();

    // A game's code can also set the mode straight after the init with movs r0, #imm; lsls r0, r0, #n before each call
    // of m4aSoundMode, after calls of up to two other routines. The calls apply in order. The cart's init sets 5
    // channels and a master volume of 12.
    struct ShiftedCase
    {
        std::vector<Cart::ShiftedMode> modes;
        int other_calls;
        int channels;
        int master_volume;
    };
    const ShiftedCase kShiftedCases[] = {
        {{{0xC0, 3}}, 0, 6, 12},            // The Proud Family: 6 channels
        {{{0xC0, 4}, {0xC0, 7}}, 1, 12, 6}, // Sugar Sugar Rune: 12 channels, then a master volume of 6
        {{{0x80, 4}, {0x80, 8}}, 2, 8, 8},
        {{{0xC0, 3}}, 3, 5, 12}, // past the calls that count
    };
    for (const ShiftedCase& c : kShiftedCases)
    {
        Cart game;
        game.GameShiftedSoundModes(c.modes, c.other_calls);
        game.Song({Track().Fine()}, voices);

        const DriverInfo game_info = Detect(game.ToRom());

        SEQLIB_CHECK_EQ(game_info.max_channels, c.channels);
        SEQLIB_CHECK_EQ(game_info.master_volume, c.master_volume);
        SEQLIB_CHECK_EQ(game_info.game_modes.size(), c.other_calls < 3 ? c.modes.size() : 0);
    }

    // A row gives the mode that Strawberry Shortcake: Sweet Dreams sets each time it starts a song.
    Cart sweet_dreams;
    sweet_dreams.SetGameCode("B4TE");
    sweet_dreams.Song({Track().Fine()}, voices);

    const DriverInfo sweet_dreams_info = Detect(sweet_dreams.ToRom());

    SEQLIB_CHECK_EQ(sweet_dreams_info.max_channels, 6);
    SEQLIB_CHECK_EQ(sweet_dreams_info.master_volume, 12);
}

// The channels that games set for each song, a compiler's swapped registers, a cartridge without the driver, and a
// player table with an entry of zeros.
void TestDetectionVariants()
{
    const uint32_t voices = SawVoices();

    // Fire Emblem and Kuru Kuru Kururin set the DirectSound channels for the song they start.
    Cart emblem;
    emblem.SetGameCode("AE7E");
    emblem.PutHalfwords(0x08003FC0,
                        {0xB580, 0xB081, 0x466F, 0x6038, 0x6838, 0x285A, 0xD010, 0x285A, 0xDC04, 0x282A, 0xDC18,
                         0x2829, 0xDB16, 0xE009, 0x285F, 0xD007, 0x285F, 0xDC02, 0x285C, 0xD003, 0xE00E, 0x2874,
                         0xD000, 0xE00B, 0x4804, 0x2108, 0x5641, 0x2908, 0xD002, 0x2008, 0xF7FF, 0xFFC6});
    emblem.Song({Track().Fine()}, voices);
    const Rom emblem_rom = emblem.ToRom();
    const DriverInfo emblem_info = Detect(emblem_rom);
    SEQLIB_CHECK_EQ(SongChannels(emblem_rom, emblem_info, 41), 8);
    SEQLIB_CHECK_EQ(SongChannels(emblem_rom, emblem_info, 40), emblem_info.max_channels);
    Cart kururin;
    kururin.SetGameCode("AKRP");
    kururin.Put32(0x0801EF6C + 4, 2);
    kururin.Song({Track().Fine()}, voices);
    const Rom kururin_rom = kururin.ToRom();
    const DriverInfo kururin_info = Detect(kururin_rom);
    SEQLIB_CHECK_EQ(SongChannels(kururin_rom, kururin_info, 1), 2);
    SEQLIB_CHECK_EQ(SongChannels(kururin_rom, kururin_info, 24), kururin_info.max_channels);
    SEQLIB_CHECK_EQ(Sequencer(kururin_rom, kururin_info, 1).DirectChannels(), 2);

    // Some games' compiler swaps r2 and r3 in the routine that starts a song. The mono mixer's volume code marks a
    // mixer that plays every channel on both sides.
    Cart swapped;
    swapped.Song({Track().Fine()}, voices);
    const uint16_t kSwapped[] = {0x4B08, 0x4909, 0x0B40, 0x1840, 0x8882, 0x0051, 0x1889, 0x0089, 0x18C9};
    for (uint32_t i = 0; i < 9; i++)
    {
        swapped.Put16(kSongStart + 4 + 2 * i, kSwapped[i]);
    }

    PutMonoMixer(swapped);

    const DriverInfo swapped_info = Detect(swapped.ToRom());

    SEQLIB_CHECK_EQ(swapped_info.song_start, kSongStart);
    SEQLIB_CHECK_EQ(swapped_info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(swapped_info.player_table, kPlayerTable);
    SEQLIB_CHECK(swapped_info.mono);

    // A cartridge without the driver's code fails, unless the song table is given.
    Cart bare(kMode, {10}, false);
    bare.Song({Track().Fine()}, voices);
    const Rom bare_rom = bare.ToRom();
    DriverOverrides table;
    table.song_table = kSongTable;
    DriverInfo none;
    // The failure clears an earlier message.
    std::string error = "an earlier message";

    SEQLIB_CHECK(!DetectDriver(bare_rom, DriverOverrides(), none, error) && error.empty());
    SEQLIB_CHECK(DetectDriver(bare_rom, table, none, error));
    SEQLIB_CHECK_EQ(none.song_count, 1);
    SEQLIB_CHECK_EQ(SongPlayer(bare_rom, none, 0).track_count, 16);
    // An entry of zeros in the player table is a player with no tracks. The players after it still count. A song plays
    // no more tracks than its player has. Song 82 of Advance Wars is such a song.
    Cart gap(kMode, {10, 1});
    for (uint32_t i = 0; i < 12; i += 4)
    {
        gap.Put32(kPlayerTable + i, 0);
    }

    gap.Song({Track().Fine(), Track().Fine()}, voices, 1);
    const Rom gap_rom = gap.ToRom();

    const DriverInfo gap_info = Detect(gap_rom);

    SEQLIB_REQUIRE_EQ(gap_info.players.size(), 2);
    SEQLIB_CHECK_EQ(gap_info.players[0].track_count, 0);
    SEQLIB_CHECK_EQ(SongPlayer(gap_rom, gap_info, 0).track_count, 1);
}

void TestUnusedPlayers()
{
    // The music player table's entry 1 is zeros: an unused player. Detection reads on past it. A song on player 2 plays
    // as many of its tracks as player 2 has room for. A song on the unused player plays nothing.
    Cart cart(kMode, {10, 3, 2});
    for (uint32_t i = 0; i < 12; i += 4)
    {
        cart.Put32(kPlayerTable + 12 + i, 0);
    }

    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    const Track track = Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 60, 127).Wait(24).Fine();
    cart.Song({track, track, track}, voices, 2);
    cart.Song({track}, voices, 1);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("unused_player");

    SEQLIB_REQUIRE_EQ(info.players.size(), 3);
    SEQLIB_CHECK(info.players[1].track_count == 0 && info.players[2].track_count == 2);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK(Sequencer(rom, info, 0).Valid() && Sequencer(rom, info, 0).TrackCount() == 2);
    SEQLIB_CHECK(!Sequencer(rom, info, 1).Valid());
    SEQLIB_CHECK(!ConvertSong(rom, info, 1, opt, nullptr).ok);
}

int RunTests()
{
    TestDecoding();
    TestDetection();
    TestSoundModes();
    TestShiftedSoundModes();
    TestDetectionVariants();
    TestUnusedPlayers();

    return seqlib::testing::Summary("mp2k_detection");
}

} // namespace
} // namespace agblib::sequence_export::mp2k

int main()
{
    return agblib::sequence_export::mp2k::RunTests();
}
