// SPDX-License-Identifier: MIT

// Unit tests for MP2K's sequencer: timing, notes, channels, envelopes, pitch and the songs' settings.
// mp2k_detection_test.cpp tests the decoding and the detection, and mp2k_loops_test.cpp the loops and the ends. They
// build small synthetic cartridges in memory. No game ROM is needed.

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include "mp2k_test_util.h"

namespace agblib::sequence_export::mp2k
{
namespace
{

void TestTiming()
{
    // The driver adds the tempo to a counter each frame, and plays a tick for each 150 in it. At the start, before a
    // tempo command, it adds 150. A tempo command of 75 sets a tempo of 150 (150 quarter notes a minute at 60 frames a
    // second): a tick is then a frame. One of 60 sets 120. A tick then takes 1.25 frames, and tick 24 is on frame 30.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(24).Note(24, 60, 100).Fine()},
              voices);
    cart.Song({Track().Cmd(kCmdTempo, 60).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(24).Note(24, 60, 100).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer fast(rom, info, 0);
    Sequencer slow(rom, info, 1);
    const std::vector<Action> fast_notes = Actions(fast, 40, Action::kNoteOn);
    const std::vector<Action> slow_notes = Actions(slow, 40, Action::kNoteOn);

    SEQLIB_CHECK(fast_notes.size() == 1 && fast_notes[0].frame == 24 && fast_notes[0].tick == 24);
    SEQLIB_CHECK(slow_notes.size() == 1 && slow_notes[0].frame == 30 && slow_notes[0].tick == 24);
}

void TestNotes()
{
    // A note of 24 ticks is released 24 ticks after it starts, a tied note at its end command, and a note with an extra
    // length that much later.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(24, 60, 100)
                   .Wait(48)
                   .Tie(62, 100)
                   .Wait(12)
                   .Eot(62)
                   .Wait(12)
                   .Note(12, 64, 100, 3)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<Action> ons, releases;
    for (int f = 0; f < 120; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kNoteOn)
            {
                ons.push_back(a);
            }

            if (a.kind == Action::kRelease)
            {
                releases.push_back(a);
            }
        }
    }

    SEQLIB_REQUIRE_EQ(ons.size(), 3);
    SEQLIB_REQUIRE_EQ(releases.size(), 3);
    SEQLIB_CHECK(ons[0].tick == 0 && ons[1].tick == 48 && ons[2].tick == 72);
    SEQLIB_CHECK(releases[0].tick == 24 && releases[1].tick == 60 && releases[2].tick == 87);
    SEQLIB_CHECK(ons[0].key == 60 && ons[0].velocity == 100 && ons[0].program == 0 && ons[0].channel == 0);
}

// The cart of the channel tests: two DirectSound channels and four tracks. Track 0, of priority 10, plays keys 60 and
// 62 and releases 60. Track 1, of priority 0, plays 64 and then 65 on the saw. Tracks 2 and 3 play 70 and 72 on square
// 1.
Rom ChannelsRom()
{
    Cart cart(0x00940200);
    const uint32_t kSquare = 0;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})},
                                             {1, Cart::Voice(1, kSquare, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdPrio, 10)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 100)
                   .Tie(62, 100)
                   .Wait(2)
                   .Eot(60)
                   .Wait(10)
                   .Fine(),
               Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Wait(1).Tie(64, 100).Wait(2).Tie(65, 100).Wait(10).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Tie(70, 100).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Wait(1).Tie(72, 100).Wait(12).Fine()},
              voices);

    return cart.ToRom();
}

void TestChannels()
{
    // Two DirectSound channels. Track 0, of priority 10, plays on both. A note of track 1, of priority 0, finds none it
    // can take. Once track 0 releases a note, track 1 takes its channel, released notes first.
    const Rom rom = ChannelsRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<Action> actions;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : seq.Step())
        {
            actions.push_back(a);
        }
    }

    std::vector<std::pair<int, int>> ons, dropped;
    for (const Action& a : actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            ons.push_back({a.key, a.channel});
        }

        if (a.kind == Action::kNoteDropped)
        {
            dropped.push_back({a.key, int(a.tick)});
        }
    }

    // Keys 60 and 62 take channels 0 and 1, and 70 takes the first PSG channel. Key 64 and track 3's 72, of the same
    // priority as 70 but from a later track, are dropped. Key 65 takes the released key 60's channel.
    const std::vector<std::pair<int, int>> kOns = {{60, 0}, {62, 1}, {70, kFirstPsgChannel}, {65, 0}};
    const std::vector<std::pair<int, int>> kDropped = {{64, 1}, {72, 1}};
    SEQLIB_CHECK(ons == kOns);
    SEQLIB_CHECK(dropped == kDropped);
}

// The song of TestChannels with the channels grown: the notes that the driver left out take grown channels after the
// console's 16. Track 1's key 64 takes DirectSound channel 16, and track 3's 72 a square 1 in PSG unit 1. Key 65 takes
// channel 0. The console does the same: key 60 there is in its release. The grown square writes the unit's registers
// the way the driver writes the console's: its duty and length, its pitch and its envelope, and its outputs in the
// unit's NR51.
void TestGrownChannels()
{
    const Rom rom = ChannelsRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    seq.GrowChannels(true);
    std::vector<Action> actions;
    std::vector<agblib::agb::RegisterWrite> unit_writes;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : seq.Step())
        {
            actions.push_back(a);
        }

        for (const agblib::agb::RegisterWrite& write : seq.SoundWrites())
        {
            if (write.address >= agblib::agb::kGrownPsgRegisters)
            {
                unit_writes.push_back(write);
            }
        }
    }

    std::vector<std::pair<int, int>> ons;
    int dropped = 0;
    for (const Action& a : actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            ons.push_back({a.key, a.channel});
        }
        dropped += a.kind == Action::kNoteDropped;
    }

    const std::vector<std::pair<int, int>> kOns = {
        {60, 0}, {62, 1}, {70, kFirstPsgChannel}, {64, kChannelCount}, {72, kChannelCount + 1}, {65, 0}};
    SEQLIB_CHECK(ons == kOns);
    SEQLIB_CHECK(dropped == 0);
    SEQLIB_REQUIRE(seq.ChannelCount() == kChannelCount + 2);
    const Channel& square = seq.GetChannel(kChannelCount + 1);
    SEQLIB_CHECK(square.part == 1 && square.unit == 1);
    SEQLIB_CHECK(seq.GetChannel(kChannelCount).part == 0);

    // The unit's NR11 takes the duty, its NR13 the low byte of key 72's pitch, and its NR51 bits 0 and 4. Unit 2 takes
    // no store.
    const auto wrote = [&](uint32_t offset, uint32_t value)
    {
        return std::any_of(unit_writes.begin(), unit_writes.end(), [&](const agblib::agb::RegisterWrite& w)
                           { return w.address == agblib::agb::kGrownPsgRegisters + offset && w.value == value; });
    };
    const uint32_t pitch = KeyToPsgFrequency(1, 72, 0);
    SEQLIB_CHECK(wrote(0x02, 0x00));
    SEQLIB_CHECK(wrote(0x04, pitch & 0xFF));
    SEQLIB_CHECK(wrote(0x21, 0x11));
    SEQLIB_CHECK(std::none_of(unit_writes.begin(), unit_writes.end(), [](const agblib::agb::RegisterWrite& w)
                              { return w.address >= agblib::agb::kGrownPsgRegisters + agblib::agb::kGrownPsgBytes; }));

    // Without growth, the song plays as TestChannels has it.
    Sequencer console(rom, info, 0);
    int console_dropped = 0;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : console.Step())
        {
            console_dropped += a.kind == Action::kNoteDropped;
        }
    }

    SEQLIB_CHECK(console_dropped == 2 && console.ChannelCount() == kChannelCount);
}

void TestEnvelope()
{
    // An attack of 100 a frame, a decay of 128/256 a frame to a sustain level of 64, and a release of 128/256 a frame
    // after the note's 96 ticks. With an echo volume of 16 and an echo length of 3, the release stays at 16 for 3
    // frames and then stops.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {100, 128, 64, 128})}});
    cart.Song(
        {Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(96, 60, 127).Wait(96).Wait(24).Fine()},
        voices);
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Raw({kCmdXcmd, kXcmdEchoVol, 16, kCmdXcmd, kXcmdEchoLen, 3})
                   .Note(96, 60, 127)
                   .Wait(96)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // The envelope's level after each frame: frames 0-4, then 96-103.
    auto levels = [&](int song)
    {
        Sequencer seq(rom, info, song);
        std::vector<int> out;
        for (int f = 0; f < 104; f++)
        {
            seq.Step();
            if (f < 5 || f >= 96)
            {
                out.push_back(seq.GetChannel(0).status ? seq.GetChannel(0).envelope : -1);
            }
        }

        return out;
    };

    const std::vector<int> kPlain = {100, 200, 255, 127, 64, 32, 16, 8, 4, 2, 1, -1, -1};
    const std::vector<int> kEcho = {100, 200, 255, 127, 64, 32, 16, 16, 16, -1, -1, -1, -1};
    SEQLIB_CHECK(levels(0) == kPlain);
    SEQLIB_CHECK(levels(1) == kEcho);
}

void TestEnvelopeHold()
{
    // The frame in which a sample note's attack reaches 255 plays at 255, and the decay starts on the next frame. The
    // SoundFont keeps the full level for that frame where the decay changes the level: below a sustain of 255. A PSG
    // voice's envelope steps on the hardware's counter instead.
    const DriverInfo info;
    Voice sample;
    sample.attack = 255;
    sample.sustain = 128;
    Voice full = sample;
    full.sustain = 255;
    Voice square;
    square.type = 1;
    square.attack = 0;
    square.decay = 2;
    square.sustain = 7;

    const VolumeEnvelope held = EnvelopeFor(info, sample);
    const VolumeEnvelope sustained = EnvelopeFor(info, full);
    const VolumeEnvelope psg = EnvelopeFor(info, square);

    SEQLIB_CHECK_EQ(held.hold, Timecents(1.0 / kFrameRate));
    SEQLIB_CHECK_EQ(held.decay, kInstant);
    SEQLIB_CHECK_EQ(sustained.hold, kInstant);
    SEQLIB_CHECK_EQ(psg.hold, kInstant);
}

void TestPsgEnvelopeBytes()
{
    // The driver counts a PSG voice's whole attack, decay and release bytes. An attack of 10 takes 10 counts a step, 16
    // counts every 15 frames. A sustain above 15 never falls below the full level. The zone keeps it.
    const DriverInfo info;
    Voice slow;
    slow.type = 2;
    slow.attack = 10;
    slow.decay = 10;
    slow.sustain = 18;
    slow.release = 10;
    Voice fast = slow;
    fast.release = 2;

    const VolumeEnvelope env = EnvelopeFor(info, slow);
    const VolumeEnvelope fast_env = EnvelopeFor(info, fast);

    SEQLIB_CHECK_EQ(env.attack, Timecents(15.0 * 10 / (kFrameRate * 16 / 15)));
    SEQLIB_CHECK_EQ(env.decay, kInstant);
    SEQLIB_CHECK_EQ(env.sustain, 0);
    SEQLIB_CHECK(env.release > fast_env.release);
}

void TestPitch()
{
    // A sample's rate for a key: its rate at key 60, in 1/1024 Hz, times 2^(n/12), with a fine tune that runs in a
    // straight line in rate to the next key.
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 60, 0), 13379);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 72, 0), 26758);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 48, 0), 6689);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 60, 128), 13776);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 66, 0), 18920);

    // The PSG's square settings: C4 is 1547, and keys up to 36 all play C2. The noise settings run from 0xD7 at key 21
    // to 0 at key 80.
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(1, 60, 0), 1547);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(1, 36, 0), 44);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(1, 20, 0), 44);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 21, 0), 0xD7);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 26, 0), 0xC6);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 77, 0), 0x03);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 90, 0), 0x00);

    // A bend of +16 with a range of 12 raises the pitch by 3 semitones, and the channel's rate follows at the end of
    // the frame.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 100)
                   .Wait(2)
                   .Cmd(kCmdBendR, 12)
                   .Cmd(kCmdBend, 0x50)
                   .Wait(4)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    seq.Step();
    const uint32_t before = seq.GetChannel(0).frequency;
    seq.Step();
    seq.Step();
    const uint32_t after = seq.GetChannel(0).frequency;

    SEQLIB_CHECK_EQ(before, 13379);
    SEQLIB_CHECK_EQ(after, KeyToFrequency(13379 * 1024, 63, 0));
}

void TestLevelReports()
{
    // A track sets volume 100 and pan 0x50, and a tick later volume 1 and pan 0x20.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 100)
                   .Cmd(kCmdPan, 0x50)
                   .Wait(1)
                   .Cmd(kCmdVol, 1)
                   .Cmd(kCmdPan, 0x20)
                   .Wait(1)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);

    const std::vector<Action> reports = Actions(seq, 4, Action::kLevels);

    // Volume 100 gives a level of 200. Pan 0x50 gives the left 95/256 of it and the right 160/256: 74 and 125, rounded
    // down. Volume 1 gives a level of 2, and pan 0x20 gives the left 191/256 of it and the right 64/256. Rounded down,
    // the left plays at 1 and the right not at all.
    SEQLIB_REQUIRE_EQ(reports.size(), 2);
    SEQLIB_CHECK(reports[0].tick == 0 && reports[0].left == 74 && reports[0].right == 125);
    SEQLIB_CHECK(reports[1].tick == 1 && reports[1].left == 1 && reports[1].right == 0);
}

void TestDroppedNotes()
{
    // With one DirectSound channel, the second of two notes at once is left out of the MIDI file. The game leaves it
    // out too. After a tempo command of 150, a frame plays two ticks. A note of one tick that starts on a frame's first
    // tick is therefore released before the mixer has started it. It never sounds, and it's left out too.
    Cart cart(0x00940100);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 150)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(24, 60, 100)
                   .Wait(25)
                   .Note(1, 64, 100)
                   .Wait(24)
                   .Fine(),
               Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 62, 100).Wait(48).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("dropped");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK(r.ok && r.warnings.size() == 1 && r.warnings[0].find("1 note had no available sound channel") == 0);
    SEQLIB_CHECK(m.Find(0x90).size() == 1 && m.Find(0x91).empty());
}

void TestSongChannels()
{
    // Before The Sacred Stones starts a song up to 127, its routine at 0x080028FC gives songs 1, 2, 64, 66, 67, 86 and
    // 116 8 DirectSound channels, and the other songs the init's number. The cart's init gives one. Song 0 then plays
    // one of its two notes at once, and song 1 plays both. A routine with other code gives no song another number.
    const std::vector<uint16_t> kRoutine = {0xB500, 0x2843, 0xDC08, 0x2842, 0xDA0A, 0x2801, 0xDB14, 0x2802, 0xDD06,
                                            0x2840, 0xD004, 0xE00F, 0x2856, 0xD001, 0x2874, 0xD10B, 0x4804, 0x7A00,
                                            0x0600, 0x1600, 0x2808, 0xD00E, 0x2008, 0xF7FF, 0xFFDD};

    auto make = [&](uint16_t first)
    {
        Cart cart(0x00940100);
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
        for (int s = 0; s < 2; s++)
        {
            cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 60, 100).Wait(24).Fine(),
                       Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 62, 100).Wait(24).Fine()},
                      voices);
        }

        cart.SetGameCode("BE8E");
        cart.PutHalfwords(0x080028FC, kRoutine);
        cart.Put16(0x080028FC, first);

        return cart.ToRom();
    };

    const Rom rom = make(kRoutine[0]);
    const Rom other_rom = make(0xB510);
    const DriverInfo info = Detect(rom);
    const DriverInfo other_info = Detect(other_rom);
    ConvertSettings opt = TempSettings("song_channels");

    const SongReport r0 = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport r1 = ConvertSong(rom, info, 1, opt, nullptr);
    const SongReport other = ConvertSong(other_rom, other_info, 1, opt, nullptr);

    SEQLIB_CHECK_EQ(SongChannels(rom, info, 0), 1);
    SEQLIB_CHECK_EQ(SongChannels(rom, info, 1), 8);
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(),
                           "songs 1, 2, 64, 66, 67, 86 and 116: 8 DirectSound channels (the game's routine at "
                           "0x080028FC)") != info.log.end());
    SEQLIB_CHECK(r0.ok && r0.warnings.size() == 1 && r0.warnings[0].find("1 note had no available sound channel") == 0);
    SEQLIB_CHECK(r1.ok && r1.warnings.empty());
    SEQLIB_CHECK(other_info.song_channels.songs.empty() && SongChannels(other_rom, other_info, 1) == 1);
    SEQLIB_CHECK(other.ok && other.warnings.size() == 1);
}

void TestSongSettings()
{
    // Steel Empire gives the player of song 1 a volume of 0x122, a volX of 72. Volume 100 then gives a level of 225.
    // Song 0's 0x100 gives 200. A pan of 0 gives the left 127/256 of the level and the right 128/256, rounded down.
    auto make = [](std::string_view code, uint8_t reverb)
    {
        Cart cart;
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
        for (int s = 0; s < 5; s++)
        {
            cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine()}, voices, 0, 0,
                      s == 2 ? reverb : 0);
        }

        cart.SetGameCode(code);

        return cart.ToRom();
    };

    const Rom steel_rom = make("BKTP", 0);
    const DriverInfo steel_info = Detect(steel_rom);
    Sequencer quiet(steel_rom, steel_info, 0);
    Sequencer loud(steel_rom, steel_info, 1);
    const std::vector<Action> quiet_levels = Actions(quiet, 1, Action::kLevels);
    const std::vector<Action> loud_levels = Actions(loud, 1, Action::kLevels);
    SEQLIB_CHECK_EQ(SongVolume(steel_info, 0), kFullPlayerVolume);
    SEQLIB_CHECK_EQ(SongVolume(steel_info, 1), 0x122);
    SEQLIB_REQUIRE(quiet_levels.size() == 1 && loud_levels.size() == 1);
    SEQLIB_CHECK(quiet_levels[0].left == 99 && quiet_levels[0].right == 100);
    SEQLIB_CHECK(loud_levels[0].left == 111 && loud_levels[0].right == 112);
    const std::string louder =
        "songs 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13 and 14: player volume 0x122 (the game's code)";
    SEQLIB_CHECK(std::find(steel_info.log.begin(), steel_info.log.end(), louder) != steel_info.log.end());

    // Mr. Driller 2 plays song 1 with a reverb of 50. Song 2's header sets a reverb of 5. The song keeps it. Song 0
    // plays with the driver's reverb.
    const Rom driller_rom = make("BR2E", 0x85);
    const DriverInfo driller_info = Detect(driller_rom);
    agblib::mp2k::SongHeader header;
    SEQLIB_REQUIRE(ReadSong(driller_rom, driller_info, 1, header));
    SEQLIB_CHECK_EQ(SongReverb(driller_info, 1, header), 50);
    SEQLIB_REQUIRE(ReadSong(driller_rom, driller_info, 2, header));
    SEQLIB_CHECK_EQ(SongReverb(driller_info, 2, header), 5);
    SEQLIB_REQUIRE(ReadSong(driller_rom, driller_info, 0, header));
    SEQLIB_CHECK_EQ(SongReverb(driller_info, 0, header), driller_info.reverb);

    // The Barbie Diaries: High School Mystery plays song 3 with 6 DirectSound channels and song 4 with 8. Song 0 keeps
    // the init's number.
    const Rom barbie_rom = make("BBIE", 0);
    const DriverInfo barbie_info = Detect(barbie_rom);
    SEQLIB_CHECK_EQ(SongChannels(barbie_rom, barbie_info, 3), 6);
    SEQLIB_CHECK_EQ(SongChannels(barbie_rom, barbie_info, 4), 8);
    SEQLIB_CHECK_EQ(SongChannels(barbie_rom, barbie_info, 0), barbie_info.max_channels);

    // Another game's songs get none of these settings.
    const Rom other_rom = make("XXXX", 0);
    const DriverInfo other_info = Detect(other_rom);
    SEQLIB_CHECK(other_info.song_settings.empty());
    SEQLIB_CHECK_EQ(SongVolume(other_info, 1), kFullPlayerVolume);
}

void TestDoubledNote()
{
    // Two notes of one key that a track starts on one tick make one MIDI note. It goes on as long as either of them
    // sounds. After a tempo command of 150, the note of one tick is released before the mixer has started it, but the
    // note of 24 ticks still plays.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 11025, 32), 60, {255, 0, 255, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 150)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 100)
                   .Wait(1)
                   .Note(1, 60, 100)
                   .Note(24, 60, 100)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("doubled");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    const auto ons = m.Find(0x90);
    const auto offs = m.Find(0x80);
    SEQLIB_CHECK(ons.size() == 1 && offs.size() == 1);
    if (ons.size() == 1 && offs.size() == 1)
    {
        SEQLIB_CHECK(ons[0].tick == kMidiTicks && ons[0].bytes[1] == 60);
        SEQLIB_CHECK_EQ(offs[0].tick, 25 * kMidiTicks);
    }
}

// Returns the microseconds a quarter note lasts when a tick takes `frames` frames.
uint32_t Micros(double frames)
{
    return uint32_t(std::lround(1e6 * 24 * frames / kFrameRate));
}

void TestTempoChange()
{
    // At a tempo of 200, a frame adds 200 to the tick counter. Tick 2 runs in frame 2 with 100 of the counter left
    // over. A change to a tempo of 50 there therefore makes tick 3 come a frame later, in frame 3, rather than 3 frames
    // later. The MIDI file's tick 2 takes the 100 at the old tempo and the other 50 at the new one: 1.5 frames.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 100)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Wait(2)
                   .Cmd(kCmdTempo, 25)
                   .Wait(1)
                   .Note(1, 60, 100)
                   .Wait(2)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("tempo");

    Sequencer seq(rom, info, 0);
    const std::vector<Action> notes = Actions(seq, 10, Action::kNoteOn);
    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(notes.size() == 1 && notes[0].tick == 3 && notes[0].frame == 3);
    const std::vector<seqlib::midi::Tempo> kTempos = {
        {.track = 0, .tick = 0, .micros_per_quarter = Micros(0.75)},
        {.track = 0, .tick = 2 * kMidiTicks, .micros_per_quarter = Micros(1.5)},
        {.track = 0, .tick = 3 * kMidiTicks, .micros_per_quarter = Micros(3)}};
    SEQLIB_CHECK(seqlib::midi::Tempos(ReadMidi(r.midi_path)) == kTempos);
    SEQLIB_CHECK(AtFrames(r.seconds, 2 * 0.75 + 1.5 + 2 * 3));
}

void TestPsgPitch()
{
    // A noise note bent up 3 semitones plays the noise setting 3 keys up, 1.6 times as fast. The MIDI file therefore
    // bends it by 8.14 semitones, with a range of 9. A square note at key 38 bent down 4 semitones plays at key 36, the
    // lowest. The MIDI file bends it by 2.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(4, 0, 60, {0, 0, 15, 0})}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdBendR, 12)
                   .Tie(60, 127)
                   .Wait(1)
                   .Cmd(kCmdBend, 0x50)
                   .Wait(4)
                   .Eot(60)
                   .Fine(),
               Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdBendR, 8)
                   .Tie(38, 127)
                   .Wait(1)
                   .Cmd(kCmdBend, 0x20)
                   .Wait(4)
                   .Eot(38)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("psg_pitch");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents m = ReadMidi(r.midi_path);
    std::map<int, int> ranges;
    for (int ch = 0; ch < 2; ch++)
    {
        for (const auto& e : m.Find(uint8_t(0xB0 | ch)))
        {
            ranges[ch] = e.bytes[1] == cc::kDataEntry ? e.bytes[2] : ranges[ch];
        }
    }

    const auto noise = m.Find(0xE0);
    const auto square = m.Find(0xE1);
    const int noise_bend = noise.empty() ? 0 : noise.back().bytes[1] | (noise.back().bytes[2] << 7);
    const int square_bend = square.empty() ? -1 : square.back().bytes[1] | (square.back().bytes[2] << 7);
    const int kExpected = 8192 + int(std::lround(256.0 * 12.0 * std::log2(1.6) * 8192 / (9 * 256)));

    SEQLIB_CHECK(ranges[0] == 9 && ranges[1] == 2);
    SEQLIB_CHECK(std::abs(noise_bend - kExpected) <= 1);
    SEQLIB_CHECK_EQ(square_bend, 0);
}

void TestPsgRelease()
{
    // A noise note bent up 3 semitones plays the noise setting 3 keys up. The MIDI file bends that up 8.14 semitones,
    // with a range of 9. Its release goes on at that pitch after the note ends on tick 6. The bend stays there until
    // the track's next note, a sample on tick 24, which plays with the track's bend of 3 semitones.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(4, 0, 60, {0, 0, 15, 4})}, {1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdBendR, 12)
                   .Cmd(kCmdBend, 0x50)
                   .Note(6, 60, 127)
                   .Wait(24)
                   .Cmd(kCmdVoice, 1)
                   .Note(12, 62, 127)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("psg_release");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const auto bends = ReadMidi(r.midi_path).Find(0xE0);
    const int kNoise = 8192 + int(std::lround(256.0 * 12.0 * std::log2(1.6) * 8192 / (9 * 256)));
    const int kSample = 8192 + int(std::lround(768.0 * 8192 / (9 * 256)));

    SEQLIB_CHECK(r.ok);
    SEQLIB_REQUIRE_EQ(bends.size(), 2);
    SEQLIB_CHECK(bends[0].tick == 0 && bends[1].tick == 24 * kMidiTicks);
    SEQLIB_CHECK(std::abs((bends[0].bytes[1] | (bends[0].bytes[2] << 7)) - kNoise) <= 1);
    SEQLIB_CHECK((bends[1].bytes[1] | (bends[1].bytes[2] << 7)) == kSample);
}

void TestQuietPsg()
{
    // A PSG note's level is its two volumes added up and divided by 16. At a velocity of 32 and a track volume of 31,
    // that rounds to 0. The note then never sounds and the MIDI file leaves it out. At a velocity of 96 it sounds.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 31)
                   .Note(12, 60, 32)
                   .Wait(12)
                   .Note(12, 62, 96)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("quiet");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const auto ons = ReadMidi(r.midi_path).Find(0x90);
    SEQLIB_CHECK(ons.size() == 1 && ons[0].tick == 12 * kMidiTicks && ons[0].bytes[1] == 62);
}

void TestSilentSongs()
{
    // A song makes no sound when each of its notes plays at a level of 0, and nothing is written for it. The test
    // cartridges' master volume of 12 plays a sample at full envelope at 207/256 of its two volumes. A sample note's
    // volumes at a track volume of 127 are about its velocity. The wave channel's level is its two volumes added up and
    // divided by 16. NR32 mutes a level of 1.
    //  - Song 0: a track volume of 0. Silent.
    //  - Song 1: a velocity of 2 at a track volume of 127 gives volumes of 1. The mixer plays them at 0. Silent.
    //  - Song 2: a velocity of 4 gives volumes of 3. The mixer plays them at 2, and the song sounds.
    //  - Song 3: a note starts at a track volume of 0 and sounds once VOL raises the volume.
    //  - Song 4: the wave channel at velocity 16 has volumes of 15 and a level of 1. Silent.
    //  - Song 5: at velocity 17, the volumes are 16 and the level is 2. NR32 plays level 2 at 25%, and the song sounds.
    //  - Song 6: track 0 at a track volume of 0 and track 1 at 127. Silent on track 0 alone.
    //  - Song 7: a sample panned hard right, at a volume of 0 on the left. It sounds.
    //  - Song 8: a sample panned hard left, at a volume of 0 on the right. It sounds.
    Cart cart;
    const uint32_t wave = cart.Place(std::vector<uint8_t>(16, 0x0F));
    const uint32_t voices = cart.VoiceGroup(
        {{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}, {1, Cart::Voice(3, wave, 60, {0, 0, 15, 0})}});

    const auto one_note = [](uint8_t voice, int volume, int velocity)
    {
        return Track().Cmd(kCmdVoice, voice).Cmd(kCmdVol, volume).Note(12, 60, velocity).Wait(12).Fine();
    };

    cart.Song({one_note(0, 0, 4)}, voices);
    cart.Song({one_note(0, 127, 2)}, voices);
    cart.Song({one_note(0, 127, 4)}, voices);
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 0).Note(48, 60, 100).Wait(24).Cmd(kCmdVol, 100).Wait(24).Fine()},
              voices);
    cart.Song({one_note(1, 127, 16)}, voices);
    cart.Song({one_note(1, 127, 17)}, voices);
    cart.Song({one_note(0, 0, 100), one_note(0, 127, 100)}, voices);
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x7F).Note(12, 60, 100).Wait(12).Fine()},
              voices);
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x00).Note(12, 60, 100).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings first_track = TempSettings("silent_track");
    first_track.track_mask = 1;

    std::vector<SongReport> reports;
    for (int s = 0; s < 9; s++)
    {
        reports.push_back(ConvertSong(rom, info, s, TempSettings("silent_" + std::to_string(s)), nullptr));
    }

    const SongReport track = ConvertSong(rom, info, 6, first_track, nullptr);

    constexpr std::array<bool, 9> kSilent = {true, true, false, false, true, false, false, false, false};
    for (size_t s = 0; s < kSilent.size(); s++)
    {
        SEQLIB_CHECK(reports[s].ok && reports[s].silent == kSilent[s] && reports[s].midi_path.empty() == kSilent[s]);
    }

    SEQLIB_CHECK(track.ok && track.silent && track.midi_path.empty());
}

void TestSilentMonoSongs()
{
    // The mono mixer plays a sample on both sides at the average of its two volumes. A song with one sample track
    // sounds at a track volume of 100 and makes no sound at 0.
    Cart cart;
    PutMonoMixer(cart);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(12, 60, 100).Wait(12).Fine()}, voices);
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 0).Note(12, 60, 100).Wait(12).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sounds = ConvertSong(rom, info, 0, TempSettings("mono_sounds"), nullptr);
    const SongReport silent = ConvertSong(rom, info, 1, TempSettings("mono_silent"), nullptr);

    SEQLIB_REQUIRE(info.mono);
    SEQLIB_CHECK(sounds.ok && !sounds.silent && !sounds.midi_path.empty());
    SEQLIB_CHECK(silent.ok && silent.silent && silent.midi_path.empty());
}

void TestPsgLevels()
{
    // At a track volume of 127, the MIDI file plays a PSG note of velocity v at level 15v/127, and the hardware at the
    // driver's whole number: its two volumes added up and divided by 16, rounded down. The MIDI velocity's square
    // follows v times the hardware's level against the MIDI file's.
    //  - A square at velocity 64 plays at level 7, not 7.56. The velocity of 59.27 gives a MIDI velocity of 87, not 90.
    //  - The wave channel at velocity 36 plays at level 4 in the driver, which NR32 plays at 25%. A pattern of 3 and 14
    //    plays as 0 and 3 at 25%, at 0.273 of the pattern's level rather than 0.25. That's level 4.09, not 4.25: a
    //    MIDI velocity of 66, not 68.
    //  - At velocity 95, the driver's level of 11 plays at 75%. A pattern of 0 and 15 plays as 0 and 11 there: level
    //    11, not 11.22. That's a MIDI velocity of 109, not 110.
    //  - At velocity 127, the wave channel plays its full volume, and the MIDI velocity stays 127.
    Cart cart;
    const uint32_t three_fourteen = cart.Place(std::vector<uint8_t>(16, 0x3E));
    const uint32_t zero_fifteen = cart.Place(std::vector<uint8_t>(16, 0x0F));
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})},
                                             {1, Cart::Voice(3, three_fourteen, 60, {0, 0, 15, 0})},
                                             {2, Cart::Voice(3, zero_fifteen, 60, {0, 0, 15, 0})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(12, 60, 64).Wait(12).Fine(),
               Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 127)
                   .Note(12, 62, 36)
                   .Wait(12)
                   .Cmd(kCmdVoice, 2)
                   .Note(12, 64, 95)
                   .Wait(12)
                   .Note(12, 65, 127)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("psg_levels");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // Each track plays on a MIDI channel of its own.
    std::map<int, int> velocities;
    const MidiEvents midi = ReadMidi(r.midi_path);
    for (const uint8_t status : {uint8_t(0x90), uint8_t(0x91)})
    {
        for (const auto& on : midi.Find(status))
        {
            velocities[on.bytes[1]] = on.bytes[2];
        }
    }

    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(velocities == (std::map<int, int>{{60, 87}, {62, 66}, {64, 109}, {65, 127}}));
}

void TestDump()
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0);
    const size_t loop = t.Here();
    t.Note(24, 60, 100).Wait(24).Goto(loop);
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const std::string path = Utf8(TempPath("mp2k_dump.txt"));
    std::string error;

    SEQLIB_CHECK(DumpSong(rom, info, 0, path, error));

    const std::vector<uint8_t> text = ReadAll(path);
    const std::string s(text.begin(), text.end());
    SEQLIB_CHECK(s.find("TEMPO 75 (150 BPM)") != std::string::npos);
    SEQLIB_CHECK(s.find("N24 key 60 vel 100") != std::string::npos);
    SEQLIB_CHECK(s.find("      24  B2 ") != std::string::npos);
}

// A voice's bytes as the test cartridge writes them (Cart::Voice and Cart::KeySplit). VoiceOfBytes reads them as
// ReadVoice does, and VoiceBytes gives them back. A key split's last 4 bytes give its key table, and those of another
// voice its envelope.
void TestVoiceBytes()
{
    Cart cart;
    const std::vector<uint8_t> sample = Cart::Voice(kVoiceFixed, 0x08123456, 64, {200, 10, 128, 30}, 0xC4, 5);
    const std::vector<uint8_t> split = cart.KeySplit(0x08200000, 50);
    const uint32_t group = cart.VoiceGroup({{0, sample}, {1, split}});
    const Rom rom = cart.ToRom();

    const auto check = [&rom](const std::vector<uint8_t>& bytes, uint32_t address)
    {
        agblib::mp2k::Voice read;
        SEQLIB_CHECK(ReadVoice(rom, address, read));
        const agblib::mp2k::Voice voice =
            agblib::mp2k::VoiceOfBytes(std::span<const uint8_t, 12>(bytes.data(), 12), address);
        SEQLIB_CHECK(voice.address == address && voice.address == read.address);
        SEQLIB_CHECK(voice.type == read.type && voice.key == read.key && voice.length == read.length);
        SEQLIB_CHECK(voice.pan_sweep == read.pan_sweep && voice.wave == read.wave && voice.key_table == read.key_table);
        SEQLIB_CHECK(voice.attack == read.attack && voice.decay == read.decay && voice.sustain == read.sustain &&
                     voice.release == read.release);
        const std::array<uint8_t, 12> written = agblib::mp2k::VoiceBytes(voice);
        SEQLIB_CHECK(std::equal(written.begin(), written.end(), bytes.begin(), bytes.end()));
        return voice;
    };

    const agblib::mp2k::Voice sampled = check(sample, group);
    SEQLIB_CHECK(sampled.type == kVoiceFixed && sampled.key == 64 && sampled.length == 5 && sampled.pan_sweep == 0xC4);
    SEQLIB_CHECK(sampled.wave == 0x08123456 && sampled.attack == 200 && sampled.decay == 10);
    SEQLIB_CHECK(sampled.sustain == 128 && sampled.release == 30 && sampled.key_table == 0x1E800AC8);
    const agblib::mp2k::Voice keys = check(split, group + 12);
    SEQLIB_CHECK(keys.type == kVoiceKeySplit && keys.wave == 0x08200000);
    SEQLIB_CHECK(keys.key_table == xsflib::LoadLe32(&split[8]));

    // A changed envelope goes into the last 4 bytes of a voice that isn't a key split. A key split writes its table.
    agblib::mp2k::Voice edited = sampled;
    edited.attack = 255;
    agblib::mp2k::Voice edited_split = keys;
    edited_split.attack = 255;

    const std::array<uint8_t, 12> edited_bytes = agblib::mp2k::VoiceBytes(edited);
    const std::array<uint8_t, 12> split_bytes = agblib::mp2k::VoiceBytes(edited_split);

    SEQLIB_CHECK(edited_bytes[8] == 255 && edited_bytes[9] == 10);
    SEQLIB_CHECK(std::equal(split_bytes.begin(), split_bytes.end(), split.begin(), split.end()));
}

int RunTests()
{
    TestTiming();
    TestNotes();
    TestChannels();
    TestGrownChannels();
    TestEnvelope();
    TestEnvelopeHold();
    TestPsgEnvelopeBytes();
    TestPitch();
    TestLevelReports();
    TestDroppedNotes();
    TestSongChannels();
    TestSongSettings();
    TestDoubledNote();
    TestTempoChange();
    TestPsgPitch();
    TestPsgRelease();
    TestQuietPsg();
    TestSilentSongs();
    TestSilentMonoSongs();
    TestPsgLevels();
    TestDump();
    TestVoiceBytes();

    return seqlib::testing::Summary("mp2k");
}

} // namespace
} // namespace agblib::sequence_export::mp2k

int main()
{
    return agblib::sequence_export::mp2k::RunTests();
}
