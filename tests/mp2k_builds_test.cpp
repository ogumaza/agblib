// SPDX-License-Identifier: MIT

// Unit tests for the builds of MP2K in Camelot's games and in Metroid: their detection, their sequencers and their
// conversions. mp2k_camelot_mixer_test.cpp tests Camelot's mixer. They build small synthetic cartridges in memory. No
// game ROM is needed.

#include "mp2k_test_util.h"

namespace agblib::sequence_export::mp2k
{
namespace
{

void TestCamelotDetection()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}});
    cart.Song({Track().Fine()}, voices);
    Cart plain;
    plain.Song({Track().Fine()}, voices);

    const DriverInfo info = Detect(cart.ToRom());
    const DriverInfo plain_info = Detect(plain.ToRom());

    SEQLIB_CHECK_EQ(info.song_start, kSongStart);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.sound_mode, kMode);
    SEQLIB_CHECK(info.camelot_sequencer && info.camelot_mixer);
    SEQLIB_CHECK(!plain_info.camelot_sequencer && !plain_info.camelot_mixer);

    // Without an echo's literals, the echo is the earlier version's, and the left side's tap is a third of a frame
    // back.
    SEQLIB_CHECK_EQ(info.camelot_output_points, 2);
    SEQLIB_CHECK_EQ(info.camelot_echo, 53);
    SEQLIB_CHECK_EQ(info.camelot_cross_echo, -8);
    SEQLIB_CHECK_EQ(info.camelot_tap, 74);
    SEQLIB_CHECK(!info.camelot_adpcm && !info.camelot_top_priority);
    SEQLIB_CHECK_EQ(info.max_channels, 5);
    // The words of the echo come before the mixer's pulse wave, and give the echo their level.
    cart.Put32(kCamelotPulse - 0x1CC, 0xFFFA0020);
    cart.Put32(kCamelotPulse - 0x1C8, 0x001FFFFA);
    SEQLIB_CHECK_EQ(Detect(cart.ToRom()).camelot_echo, 32);
}

// Camelot's note start takes the third free channel, or the last free one when there are only one or two, unless a
// released note of the same track has a channel. It works out the left side's level from 128 rather than 127, and
// rounds a fine-tuned key's rate a little differently. Its LFO counts its delay down at a speed of 0 too.
void TestCamelotSequencer()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 127)
                   .Tie(62, 127)
                   .Tie(64, 127)
                   .Tie(65, 127)
                   .Wait(1)
                   .Eot(60)
                   .Wait(1)
                   .Tie(67, 127)
                   .Wait(1)
                   .Tie(69, 127)
                   .Wait(10)
                   .Fine()},
              voices);
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdMod, 30)
                   .Cmd(kCmdLfoDl, 4)
                   .Cmd(kCmdLfoS, 0)
                   .Tie(60, 127)
                   .Wait(2)
                   .Cmd(kCmdLfoS, 32)
                   .Wait(10)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<std::pair<int, int>> ons;
    int left = -1;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kNoteOn)
            {
                ons.push_back({a.key, a.channel});
            }
        }

        if (f == 0)
        {
            left = seq.GetChannel(2).left;
        }
    }

    Sequencer lfo(rom, info, 1);
    std::vector<Action> pitches = Actions(lfo, 12, Action::kPitch);
    pitches.erase(std::remove_if(pitches.begin(), pitches.end(), [](const Action& a) { return a.value == 0; }),
                  pitches.end());

    // Of 5 channels, keys 60, 62 and 64 take the third free one each time, and 65 the last of the two left. Once 60 is
    // released, 67 takes its channel rather than the free channel 0. Key 69 takes channel 0.
    const std::vector<std::pair<int, int>> kOns = {{60, 2}, {62, 3}, {64, 4}, {65, 1}, {67, 2}, {69, 0}};
    SEQLIB_CHECK(ons == kOns);
    SEQLIB_CHECK_EQ(left, 125);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 11, 255), 836);
    SEQLIB_CHECK_EQ(CamelotKeyToFrequency(13379 * 1024, 11, 255), 835);
    SEQLIB_CHECK_EQ(CamelotKeyToFrequency(13379 * 1024, 60, 0), 13379);

    // The LFO's delay of 4 ticks has run out by tick 4, two ticks after its speed is set: a depth of 30 on the first
    // step of 32 bends the pitch by 15/16 of a semitone.
    SEQLIB_CHECK(!pitches.empty() && pitches[0].tick == 4 && pitches[0].value == 240);
}

// A pulse or saw synth voice's SoundFont instrument has a zone for each key that the song plays it at, with a sample
// made for that key at the mixer's rate. A triangle wave's has one cycle for every key. The MIDI file's reverb is the
// echo of Camelot's mixer, whatever the song sets, and a PSG voice is quieter against the samples.
void TestCamelotConversion()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t pulse = cart.Synth(0, {0x80, 0x00, 0x00, 0x00});
    const uint32_t triangle = cart.Synth(2, {});
    const uint32_t voices = cart.VoiceGroup(
        {{0, Cart::Voice(0, pulse)}, {1, Cart::Voice(0, triangle)}, {2, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(12, 60, 127)
                   .Wait(12)
                   .Note(12, 64, 127)
                   .Wait(12)
                   .Note(12, 60, 127)
                   .Wait(12)
                   .Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Note(12, 48, 127).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 2).Cmd(kCmdVol, 127).Note(12, 72, 127).Wait(12).Fine()},
              voices, 0, 0, 0x80 | 10);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 0).Note(12, 60, 127).Wait(12).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("camelot");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport silent = ConvertSong(rom, info, 1, TempSettings("camelot_silent"), nullptr);
    const std::vector<int16_t> points = RenderSynth(rom, info, Synth::kPulse, pulse + 16, 13379, 64);

    // Camelot's mixer leaves out a note at a volume of 0 on both sides. A song of such notes makes no sound.
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(silent.ok && silent.silent && silent.midi_path.empty());
    const MidiEvents m = ReadMidi(r.midi_path);
    int reverbs = 0;
    for (uint8_t ch = 0; ch < 3; ch++)
    {
        for (const auto& e : m.Find(uint8_t(0xB0 + ch)))
        {
            if (e.bytes[1] == cc::kReverb)
            {
                reverbs++;
                SEQLIB_CHECK_EQ(e.bytes[2], 53);
            }
        }
    }

    SEQLIB_CHECK_EQ(reverbs, 3);

    // The SoundFont's instrument for each program.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    std::map<int, int> instrument;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        instrument[Le16(sf.Record("phdr", 38, p) + 20)] = int(RecordedInstrument(sf, p));
    }

    // Each zone's key range, its sample's name, rate and root key, and its attenuation.
    auto zones = [&](int program)
    {
        const size_t first = Le16(sf.Record("inst", 22, size_t(instrument[program])) + 20);
        const size_t last = Le16(sf.Record("inst", 22, size_t(instrument[program]) + 1) + 20);
        std::vector<std::map<uint16_t, uint16_t>> out;
        for (size_t z = first + 1; z < last; z++)
        {
            out.push_back(sf.ZoneGens(z));
        }

        return out;
    };

    auto sample_name = [&](const std::map<uint16_t, uint16_t>& zone)
    {
        const uint8_t* h = sf.Record("shdr", 46, zone.at(sf2::kSampleId));
        return std::string(reinterpret_cast<const char*>(h), std::find(h, h + 20, 0) - h);
    };

    auto sample_rate = [&](const std::map<uint16_t, uint16_t>& zone)
    {
        return agblib::testing::agb_files::Le32(sf.Record("shdr", 46, zone.at(sf2::kSampleId)) + 36);
    };

    auto root_key = [&](const std::map<uint16_t, uint16_t>& zone)
    {
        return int(sf.Record("shdr", 46, zone.at(sf2::kSampleId))[40]);
    };

    // The names of the pulse wave's samples for keys 60 and 64, and of the triangle wave's.
    char pulse_60[32], pulse_64[32], triangle_name[32];
    std::snprintf(pulse_60, sizeof pulse_60, "Pulse %08X 60", unsigned(pulse));
    std::snprintf(pulse_64, sizeof pulse_64, "Pulse %08X 64", unsigned(pulse));
    std::snprintf(triangle_name, sizeof triangle_name, "Triangle %08X", unsigned(triangle));

    const auto pulse_zones = zones(0);
    SEQLIB_REQUIRE_EQ(pulse_zones.size(), 2);
    SEQLIB_CHECK(pulse_zones[0].at(sf2::kKeyRange) == 0x3C3C && sample_name(pulse_zones[0]) == pulse_60);
    SEQLIB_CHECK(pulse_zones[1].at(sf2::kKeyRange) == 0x4040 && sample_name(pulse_zones[1]) == pulse_64);
    SEQLIB_CHECK(sample_rate(pulse_zones[1]) == 13379 && root_key(pulse_zones[1]) == 64);
    SEQLIB_CHECK(!pulse_zones[0].count(sf2::kInitialAttenuation) && pulse_zones[0].count(sf2::kSampleModes));
    const auto triangle_zones = zones(1);
    SEQLIB_CHECK(triangle_zones.size() == 1 && triangle_zones[0].at(sf2::kKeyRange) == 0x7F00 &&
                 sample_name(triangle_zones[0]) == triangle_name && sample_rate(triangle_zones[0]) == 13379);
    // Camelot's mixer plays a sample at 9/8 of the SDK mixer's level. The square's sample is that much quieter than the
    // SDK's: 6.3 dB and 1 dB more below a full-scale sample.
    const auto square_zones = zones(2);
    SEQLIB_CHECK(!square_zones.empty() && !square_zones[0].count(sf2::kInitialAttenuation));
    if (!square_zones.empty())
    {
        const double decibels = 20 * std::log10(sf.SamplePeak(square_zones[0].at(sf2::kSampleId)) / 32767.0);
        SEQLIB_CHECK(std::fabs(decibels + 6.3 + 20 * std::log10(9.0 / 8.0)) < 0.01);
    }

    // A pulse wave with a duty of 128/256 at the mixer's rate: 32 points high and 32 low, at half the full level.
    SEQLIB_CHECK(points.size() == 64 && points[0] == 16384 && points[31] == 16384 && points[32] == -16384 &&
                 points[63] == -16384);
}

// The Metroid games' sequencer is found by its TEMPO handler. Metroid: Zero Mission has no routine that starts a song:
// its tables come from two loads of literals in a row. The table of PSG frequency settings comes from the routine that
// reads it.
void TestMetroidDetection()
{
    Cart cart(kMode, {10, 3}, false, false);
    const uint32_t psg_table = cart.WriteMetroid(16);
    cart.Song({Track().Fine()}, cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}}));
    Cart plain(kMode, {10, 3}, false, false);
    plain.Song({Track().Fine()}, plain.VoiceGroup({{0, Cart::Voice(0, plain.Wave(Saw()))}}));

    const DriverInfo info = Detect(cart.ToRom());
    DriverInfo plain_info;
    std::string error;
    const bool plain_found = DetectDriver(plain.ToRom(), DriverOverrides(), plain_info, error);

    SEQLIB_CHECK(info.metroid_sequencer);
    SEQLIB_CHECK_EQ(info.song_start, 0u);
    SEQLIB_CHECK_EQ(info.player_table, kPlayerTable);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.song_count, 1);
    SEQLIB_CHECK_EQ(info.metroid_psg_table, psg_table);
    SEQLIB_CHECK(!plain_found && !plain_info.metroid_sequencer && !plain_info.metroid_psg_table);
}

// The Metroid games' sequencer keeps each note of a drum kit at the pan that it started with when the track's volume
// changes. Its mixer ends a note whose level reaches 0, unless the track gave both an echo volume and an echo length.
// The echo then plays at the note's last level for the length less 1. A square takes its setting from the game's table.
// In Metroid Fusion, a note reads a key and a velocity, and a third byte below 0x80 is the next note's key. In Metroid:
// Zero Mission, the third byte adds to the note's length.
void TestMetroidSequencer()
{
    Cart cart;
    cart.WriteMetroid(16);
    const uint32_t wave = cart.Wave(Saw(), 13379, 0);
    const uint32_t kit = cart.VoiceGroup({{60, Cart::Voice(0, wave, 60, {255, 0, 255, 0}, 0xA0)},
                                          {61, Cart::Voice(0, wave, 60, {255, 0, 255, 0}, 0xE0)}});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kVoiceDrumKit, kit)},
                                             {1, Cart::Voice(0, wave, 60, {255, 0, 255, 0})},
                                             {2, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 127)
                   .Tie(61, 127)
                   .Wait(1)
                   .Cmd(kCmdVol, 64)
                   .Wait(2)
                   .Fine()},
              voices);
    for (const int length : {0, 5})
    {
        cart.Song({Track()
                       .Cmd(kCmdTempo, 75)
                       .Cmd(kCmdVoice, 1)
                       .Cmd(kCmdVol, 127)
                       .Raw({kCmdXcmd, kXcmdEchoVol, 70, kCmdXcmd, kXcmdEchoLen, uint8_t(length)})
                       .Note(2, 60, 127)
                       .Wait(20)
                       .Fine()},
                  voices);
    }

    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 2).Cmd(kCmdTune, 0x48).Tie(60, 127).Wait(2).Fine()}, voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Note(24, 60, 100, 50).Wait(24).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // After the volume falls, the note of key 60 stays on the left and that of key 61 on the right.
    Sequencer drums(rom, info, 0);
    drums.Step();
    drums.Step();
    std::vector<std::pair<int, int>> sides;
    for (int c = 0; c < 2; c++)
    {
        const Channel& ch = drums.GetChannel(c);
        sides.push_back({ch.midi_key, ch.left - ch.right});
    }

    std::sort(sides.begin(), sides.end());
    SEQLIB_CHECK(sides.size() == 2 && sides[0].first == 60 && sides[0].second > 0 && sides[1].first == 61 &&
                 sides[1].second < 0);

    // The note's release takes its level to 0 in a frame. Without an echo length it ends there. With 5, it stays on for
    // 4 frames more at the level it had before.
    std::vector<int> frames;
    std::vector<int> levels;
    for (int song = 1; song <= 2; song++)
    {
        Sequencer seq(rom, info, song);
        int on = 0;
        for (int f = 0; f < 12; f++)
        {
            seq.Step();
            const Channel& ch = seq.GetChannel(0);
            if (ch.status & agblib::mp2k::kStatusOn)
            {
                on++;
                if (ch.status & agblib::mp2k::kStatusEcho)
                {
                    levels.push_back(ch.envelope);
                }
            }
        }
        frames.push_back(on);
    }

    SEQLIB_CHECK(frames.size() == 2 && frames[1] == frames[0] + 5);
    SEQLIB_CHECK(levels.size() == 5 && std::all_of(levels.begin(), levels.end(), [](int l) { return l == 255; }));

    // A tune of 8/64 of a semitone takes 129/256 of the step from key 60's setting to key 61's.
    Sequencer square(rom, info, 3);
    square.Step();
    SEQLIB_CHECK_EQ(square.GetChannel(kFirstPsgChannel).frequency, 60u * 16 + 8);

    Sequencer notes(rom, info, 4);
    std::vector<int> keys;
    for (const Action& a : Actions(notes, 1, Action::kNoteOn))
    {
        keys.push_back(a.key);
    }

    SEQLIB_CHECK(!info.metroid_short_notes && keys == std::vector<int>({60}));

    Cart fusion;
    fusion.WriteMetroid(16, true);
    fusion.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Note(24, 60, 100, 50).Wait(24).Fine()},
                fusion.VoiceGroup({{0, Cart::Voice(0, fusion.Wave(Saw(), 13379, 0))}}));
    const Rom fusion_rom = fusion.ToRom();
    const DriverInfo fusion_info = Detect(fusion_rom);
    Sequencer short_notes(fusion_rom, fusion_info, 0);
    keys.clear();
    for (const Action& a : Actions(short_notes, 1, Action::kNoteOn))
    {
        keys.push_back(a.key);
    }

    SEQLIB_CHECK(fusion_info.metroid_short_notes && keys == std::vector<int>({60, 50}));

    // The scan of a track reads the same two notes, each of 24 ticks, where a wait of 96 ticks follows them. The SDK's
    // library would read one note of 74 ticks.
    Cart longer;
    longer.WriteMetroid(16, true);
    longer.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Note(24, 60, 100, 50).Wait(96).Fine()},
                longer.VoiceGroup({{0, Cart::Voice(0, longer.Wave(Saw(), 13379, 0))}}));
    const Rom longer_rom = longer.ToRom();
    const DriverInfo longer_info = Detect(longer_rom);
    agblib::mp2k::SongHeader longer_header;
    SEQLIB_REQUIRE(ReadSong(longer_rom, longer_info, 0, longer_header));
    const uint32_t track = longer_header.tracks[0];
    SEQLIB_CHECK_EQ(ScanTrack(longer_rom, track, NoteArguments(longer_info)).last_command, 24u);
    SEQLIB_CHECK_EQ(ScanTrack(longer_rom, track).last_command, 74u);
}

// The Metroid games' PSG routine counts a square's envelope a level every `attack`, `decay` or `release` frames, with
// no extra step every 15th frame, and writes NR12 (0x63) only when a phase starts, each time with a restart in NR14
// (0x65). The attack restarts the channel at 0, rising every 6/64 s, while the count starts at 1. When the count
// reaches the full level, the decay restarts the channel a level below it, falling. The sustain restarts it at the
// sustain level with a step time of 0. The release restarts it a level below the count, falling every 2/64 s, and the
// note ends when the count reaches 0: NR12 = 8, then NR14 = 0x80. A change of volume restarts the channel at the count
// with a step time of 0. Metroid Fusion and Metroid: Zero Mission play these stores as mGBA 0.11.0 records them.
void TestMetroidPsgEnvelope()
{
    Cart cart;
    cart.WriteMetroid(16);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {6, 6, 4, 2})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 64).Note(96, 60, 127).Wait(96).Fine()},
              voices);
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 64)
                   .Note(96, 60, 127)
                   .Wait(10)
                   .Cmd(kCmdVol, 32)
                   .Wait(86)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // The frames that store to NR12, with the value and whether NR14 restarts the channel in the same frame.
    struct Envelope
    {
        int frame;
        uint32_t nr12;
        bool restart;
        Channel channel;
    };
    const auto envelopes = [&](int song)
    {
        Sequencer seq(rom, info, song);
        std::vector<Envelope> out;
        for (int f = 0; f < 140; f++)
        {
            seq.Step();
            std::optional<uint32_t> nr12;
            bool restart = false;
            for (const agblib::agb::RegisterWrite& w : seq.SoundWrites())
            {
                nr12 = (w.address & 0xFF) == 0x63 ? std::optional<uint32_t>(w.value) : nr12;
                restart = restart || ((w.address & 0xFF) == 0x65 && (w.value & 0x80));
            }
            if (nr12)
            {
                out.push_back({f, *nr12, restart, seq.GetChannel(kFirstPsgChannel)});
            }
        }

        return out;
    };

    const std::vector<Envelope> plain = envelopes(0);
    SEQLIB_REQUIRE(plain.size() >= 4);
    const int goal = plain[0].channel.goal;
    const int sustain = plain[0].channel.sustain_goal;
    SEQLIB_REQUIRE(goal > sustain + 2 && sustain > 1);
    SEQLIB_CHECK(plain[0].nr12 == 0x0E && plain[0].restart && plain[0].channel.envelope == 1);
    SEQLIB_CHECK(plain[1].frame == plain[0].frame + 6 * (goal - 1) && plain[1].restart);
    SEQLIB_CHECK_EQ(plain[1].nr12, uint32_t(((goal - 1) << 4) | 6));
    SEQLIB_CHECK(plain[2].frame == plain[1].frame + 6 * (goal - 1 - sustain) && plain[2].restart);
    SEQLIB_CHECK_EQ(plain[2].nr12, uint32_t(sustain << 4));
    SEQLIB_CHECK_EQ(plain[3].nr12, uint32_t(((sustain - 1) << 4) | 2));
    SEQLIB_CHECK(plain[3].restart && plain[3].frame == plain[0].frame + 96);
    SEQLIB_REQUIRE_EQ(plain.size(), 5);
    SEQLIB_CHECK(plain[4].nr12 == 0x08 && plain[4].frame == plain[3].frame + 2 * (sustain - 1));
    SEQLIB_CHECK_EQ(plain[4].channel.status, 0);

    // The fall of volume in the attack keeps the channel at the count, 2 after 10 frames. The decay then starts from
    // the new full level.
    const std::vector<Envelope> quieter = envelopes(1);
    SEQLIB_REQUIRE(quieter.size() >= 2);
    SEQLIB_CHECK(quieter[1].frame == quieter[0].frame + 10 && quieter[1].nr12 == 0x20 && quieter[1].restart);
    const int new_goal = quieter[1].channel.goal;
    SEQLIB_CHECK(new_goal < goal);
    SEQLIB_REQUIRE(quieter.size() >= 3);
    SEQLIB_CHECK_EQ(quieter[2].nr12, uint32_t(((new_goal - 1) << 4) | 6));
}

// A song of the Metroid games' sequencer makes no sound when each of its notes plays at a level of 0.
//  - Song 0: a sample at VOL 6 and velocity 12, as in Metroid: Zero Mission's song 51. The channel's volumes round to
//  0.
//    Silent.
//  - Song 1: the sample at VOL 100 and velocity 100. It sounds.
//  - Song 2: a square with an attack, for one frame. The count reaches level 1, a level ahead of the hardware, which
//    plays it at 0. The release then falls from 0. Silent.
//  - Song 3: the square for 12 frames. The attack reaches the hardware, and the song sounds.
void TestMetroidSilentSongs()
{
    Cart cart;
    cart.WriteMetroid(16);
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}, {1, Cart::Voice(1, 2, 60, {6, 6, 4, 2})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 6).Note(12, 60, 12).Wait(12).Fine()}, voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(12, 60, 100).Wait(12).Fine()},
              voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Note(1, 60, 127).Wait(12).Fine()},
              voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Note(12, 60, 127).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    std::vector<SongReport> reports;
    for (int s = 0; s < 4; s++)
    {
        reports.push_back(ConvertSong(rom, info, s, TempSettings("metroid_silent_" + std::to_string(s)), nullptr));
    }

    // The tracks that the sequencer marks as sounding in a song's first 30 frames.
    const auto sounded = [&](int song)
    {
        Sequencer seq(rom, info, song);
        for (int f = 0; f < 30; f++)
        {
            seq.Step();
        }

        return seq.SoundedTracks();
    };

    SEQLIB_REQUIRE(info.metroid_sequencer);
    constexpr std::array<bool, 4> kSilent = {true, false, true, false};
    for (size_t s = 0; s < kSilent.size(); s++)
    {
        SEQLIB_CHECK(reports[s].ok && reports[s].silent == kSilent[s] && reports[s].midi_path.empty() == kSilent[s]);
    }

    SEQLIB_CHECK_EQ(sounded(2), 0);
    SEQLIB_CHECK_EQ(sounded(3), 1);
}

// The Metroid games' LFO leaves its swing as it is when the triangle wave's value, before the depth scales it, equals
// the old swing: at a speed of 32 and a depth of 63, the swing goes 16, 32, 32 and 0 rather than 16, 32, 16 and 0.
void TestMetroidLfo()
{
    Cart cart;
    cart.WriteMetroid(16);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 0})}});
    cart.Song(
        {Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdLfoS, 63).Cmd(kCmdMod, 63).Tie(60, 127).Wait(6).Fine()},
        voices);
    const Rom rom = cart.ToRom();
    Sequencer seq(rom, Detect(rom), 0);

    // The swing moves the pitch by 48/256 of a semitone a step. The note at tick 0 sets the swing to 0.
    std::vector<std::pair<uint64_t, int>> pitches;
    for (const Action& a : Actions(seq, 8, Action::kPitch))
    {
        if (a.tick >= 1 && a.tick <= 4)
        {
            pitches.push_back({a.tick, a.value});
        }
    }

    const std::vector<std::pair<uint64_t, int>> kPitches = {{1, 768}, {2, 1536}, {4, 0}};
    SEQLIB_CHECK(pitches == kPitches);
}

int RunTests()
{
    TestCamelotDetection();
    TestCamelotSequencer();
    TestCamelotConversion();
    TestMetroidDetection();
    TestMetroidSequencer();
    TestMetroidLfo();
    TestMetroidPsgEnvelope();
    TestMetroidSilentSongs();

    return seqlib::testing::Summary("mp2k_builds");
}

} // namespace
} // namespace agblib::sequence_export::mp2k

int main()
{
    return agblib::sequence_export::mp2k::RunTests();
}
