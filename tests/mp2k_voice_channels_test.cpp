// SPDX-License-Identifier: MIT

// Unit tests for MP2K's conversion with a channel for each voice, and for the PSG's zones in the SoundFont: their
// exclusive classes and their reverb send. They build small synthetic cartridges in memory. No game ROM is needed.

#include "mp2k_test_util.h"

namespace agblib::sequence_export::mp2k
{
namespace
{

void TestVoiceChannels()
{
    // Track 0 plays a note with a slow release and turns its volume down while the note is held, and again while it
    // fades. Track 1 then plays a chord of 5 notes, with another voice. The last of them takes the channel of track 0's
    // note, which is still fading.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})},
                                             {2, Cart::Voice(0, cart.Wave(Saw(80), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 127)
                   .Note(12, 60, 100)
                   .Wait(6)
                   .Cmd(kCmdVol, 80)
                   .Wait(12)
                   .Cmd(kCmdVol, 50)
                   .Wait(30)
                   .Fine(),
               Track()
                   .Wait(24)
                   .Cmd(kCmdVoice, 2)
                   .Cmd(kCmdVol, 100)
                   .Note(24, 62, 100)
                   .Note(24, 64, 100)
                   .Note(24, 65, 100)
                   .Note(24, 67, 100)
                   .Note(24, 69, 100)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("voices");
    opt.voice_channels = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // A track for each of the driver's 5 channels, named after it, after the conductor track.
    SEQLIB_CHECK(r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 6);
    std::vector<std::string> names;
    for (size_t t = 1; t < m.tracks.size(); t++)
    {
        const std::vector<uint8_t>& e = m.tracks[t].events.front().bytes;
        names.push_back(std::string(e.begin() + 3, e.end()));
    }

    SEQLIB_CHECK((names == std::vector<std::string>{"DirectSound 1", "DirectSound 2", "DirectSound 3", "DirectSound 4",
                                                    "DirectSound 5"}));

    // The first channel's events, without its track's meta events.
    std::vector<seqlib::midi::Event> first;
    for (const auto& e : m.tracks[1].events)
    {
        if (e.bytes[0] < 0xF0)
        {
            first.push_back(e);
        }
    }

    // Returns where an event comes among them, or -1 if it isn't there.
    auto find = [&](uint32_t tick, std::vector<uint8_t> bytes)
    {
        for (size_t i = 0; i < first.size(); i++)
        {
            if (first[i].tick == tick * kMidiTicks && first[i].bytes == bytes)
            {
                return int(i);
            }
        }

        return -1;
    };

    // On the first channel, track 0's note is released at tick 12. The note follows its track's volume before and
    // after that, at the centre: volumes of 127, 80 and 50 give CC11 127, 100 and 79. At tick 24 an All Sound Off cuts
    // off its release. The channel then takes track 1's voice and volume for the chord's last note. A velocity of 100
    // is written as 127 × √(100 / 127), rounded: 113.
    SEQLIB_CHECK(find(0, {0xC0, 1}) >= 0 && find(0, {0x90, 60, 113}) >= 0 && find(12, {0x80, 60, 0}) >= 0);
    SEQLIB_CHECK(find(0, {0xB0, cc::kVolume, 127}) >= 0 && find(0, {0xB0, cc::kPan, 64}) >= 0);
    SEQLIB_CHECK(find(0, {0xB0, cc::kExpression, 127}) >= 0 && find(6, {0xB0, cc::kExpression, 100}) >= 0 &&
                 find(18, {0xB0, cc::kExpression, 79}) >= 0);
    const int cut = find(24, {0xB0, cc::kAllSoundOff, 0});
    const int chord = find(24, {0x90, 69, 113});
    SEQLIB_CHECK(cut >= 0 && chord > cut);
    SEQLIB_CHECK(find(24, {0xC0, 2}) >= 0 && find(24, {0xB0, cc::kExpression, 112}) >= 0);

    // The chord's other notes take the 4 free channels.
    for (uint8_t ch = 1; ch < 5; ch++)
    {
        const auto ons = m.Find(uint8_t(0x90 | ch));
        SEQLIB_CHECK(ons.size() == 1 && ons[0].tick == 24 * kMidiTicks);
    }
}

void TestVoiceChannelsDrums()
{
    // A sample voice for each of the driver's 12 DirectSound channels, and a voice for each of its 4 PSG channels.
    Cart cart(0x0094CC00, {16});
    std::map<int, std::vector<uint8_t>> voice_map;
    for (int v = 0; v < 12; v++)
    {
        voice_map[v] = Cart::Voice(0, cart.Wave(Saw(size_t(40 + 4 * v)), 13379, 0));
    }

    voice_map[12] = Cart::Voice(1, 2, 60, {0, 0, 15, 0});
    voice_map[13] = Cart::Voice(2, 2, 60, {0, 0, 15, 0});
    voice_map[14] = Cart::Voice(3, cart.Place(std::vector<uint8_t>(16, 0x5A)), 60, {0, 0, 15, 0});
    voice_map[15] = Cart::Voice(4, 0, 60, {0, 0, 15, 0});
    const uint32_t voices = cart.VoiceGroup(voice_map);

    // A song whose 16 tracks play them all at once. The 16th channel, the noise channel, therefore gets the drum
    // channel.
    std::vector<Track> tracks;
    for (int t = 0; t < 16; t++)
    {
        tracks.push_back(Track().Cmd(kCmdVoice, t).Cmd(kCmdVol, 127).Note(24, 60, 100).Wait(24).Fine());
    }

    cart.Song(tracks, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("voice_drums");
    opt.voice_channels = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The noise note plays on channel 10, and its voice has a copy in the drum bank.
    SEQLIB_CHECK(r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 17);
    const auto drum_notes = m.Find(0x99);
    SEQLIB_CHECK(drum_notes.size() == 1 && drum_notes[0].bytes[1] == 60);
    const Sf2Records sf = ReadSf2(r.sf2_path);
    bool drum_copy = false;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        drum_copy = drum_copy || (Le16(h + 22) == 128 && Le16(h + 20) == 15);
    }

    SEQLIB_CHECK(drum_copy);
}

void TestVoiceChannelsLoop()
{
    // Two voices that play on square channel 1.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}, {2, Cart::Voice(1, 1, 60, {0, 0, 15, 0})}});

    // Track 0 plays program 1 at volume 100 on tick 24, and every 48 ticks after that, looping from the start. Track 1
    // plays program 2 at volume 60 on ticks 0 and 12, and every 48 ticks after that, looping from tick 6. So the loop
    // runs from tick 6 to tick 54, and ends with track 0's settings on the channel.
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100);
    const size_t first_loop = first.Here();
    first.Wait(24).Note(12, 48, 100).Wait(24).Goto(first_loop);
    Track second;
    second.Cmd(kCmdVoice, 2).Cmd(kCmdVol, 60).Note(12, 52, 100).Wait(6);
    const size_t second_loop = second.Here();
    second.Wait(6).Note(12, 52, 100).Wait(42).Goto(second_loop);
    cart.Song({first, second}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("voice_loop");
    opt.voice_channels = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The loop's first note, at tick 12, sets the channel's program, CC10, CC11 and pitch bend again. The note before
    // it left them the same. A player that jumps back to the loop's start comes from track 0's note. Both tracks play
    // at the centre. CC11 is 87 for volume 60 and 112 for volume 100.
    using Events = std::vector<seqlib::midi::Event>;
    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(AtFrames(r.loop_start, 6));
    SEQLIB_CHECK(AtFrames(r.loop_end, 54));
    const MidiEvents m = ReadMidi(r.midi_path);
    Events levels;
    for (const auto& e : m.Find(0xB0))
    {
        if (e.bytes[1] == cc::kPan || e.bytes[1] == cc::kExpression)
        {
            levels.push_back(e);
        }
    }

    SEQLIB_CHECK((m.Find(0xC0) == Events{{0, {0xC0, 2}},
                                         {12 * kMidiTicks, {0xC0, 2}},
                                         {24 * kMidiTicks, {0xC0, 1}},
                                         {60 * kMidiTicks, {0xC0, 2}},
                                         {72 * kMidiTicks, {0xC0, 1}}}));
    SEQLIB_CHECK((levels == Events{{0, {0xB0, cc::kPan, 64}},
                                   {0, {0xB0, cc::kExpression, 87}},
                                   {12 * kMidiTicks, {0xB0, cc::kPan, 64}},
                                   {12 * kMidiTicks, {0xB0, cc::kExpression, 87}},
                                   {24 * kMidiTicks, {0xB0, cc::kExpression, 112}},
                                   {60 * kMidiTicks, {0xB0, cc::kExpression, 87}},
                                   {72 * kMidiTicks, {0xB0, cc::kExpression, 112}}}));
    SEQLIB_CHECK((m.Find(0xE0) == Events{{12 * kMidiTicks, {0xE0, 0x00, 0x40}}}));
}

// Calls `zone(program, z)` for each zone `z` after the global zone of the instrument that each preset of bank 0 plays.
template <typename Zone>
void ForBankZeroZones(const Sf2Records& sf, const Zone& zone)
{
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        if (Le16(h + 22) != 0)
        {
            continue;
        }

        const size_t instrument = RecordedInstrument(sf, p);
        const size_t first = Le16(sf.Record("inst", 22, instrument) + 20);
        const size_t end = Le16(sf.Record("inst", 22, instrument + 1) + 20);
        for (size_t z = first + 1; z < end; z++)
        {
            zone(int(Le16(h + 20)), z);
        }
    }
}

// Returns the zones of the instrument that each preset of bank 0 plays, by the preset's program: for each zone after
// the global zone, its key range and its exclusive class, or 0 for none.
std::map<int, std::vector<std::pair<int, int>>> ExclusiveClasses(const Sf2Records& sf)
{
    std::map<int, std::vector<std::pair<int, int>>> out;
    ForBankZeroZones(sf,
                     [&](int program, size_t z)
                     {
                         const std::map<uint16_t, uint16_t> gens = sf.ZoneGens(z);
                         const auto found = gens.find(sf2::kExclusiveClass);
                         out[program].push_back({gens.at(sf2::kKeyRange), found != gens.end() ? found->second : 0});
                     });

    return out;
}

// Returns true if each zone in `zones` has the exclusive class `expected`.
bool AllOfClass(const std::vector<std::pair<int, int>>& zones, int expected)
{
    return !zones.empty() &&
           std::all_of(zones.begin(), zones.end(), [&](const auto& zone) { return zone.second == expected; });
}

// A test cartridge, and the driver that detection finds in it.
struct ExclusiveSong
{
    Rom rom;
    DriverInfo info;
};

// Returns a cartridge whose song plays programs 0 to 9: a voice of each PSG channel with a slow release, a square 1
// voice with the fixed-frequency bit, two square 1 voices with quick releases, a sample with a slow release, a drum kit
// and a key split.
ExclusiveSong MakeExclusiveSong()
{
    Cart cart;
    const uint32_t wave = cart.Place(std::vector<uint8_t>(16, 0x5A));
    const uint32_t sample = cart.Wave(Saw(), 13379, 0);
    const uint32_t kit = cart.VoiceGroup({{36, Cart::Voice(4, 0, 50, {0, 0, 15, 3})},
                                          {38, Cart::Voice(4, 0, 50, {0, 0, 15, 0})},
                                          {40, Cart::Voice(0, sample, 60, {255, 0, 255, 250})}});
    const uint32_t split = cart.VoiceGroup({{0, Cart::Voice(2, 1, 60, {0, 0, 15, 5})}, {1, Cart::Voice(0, sample)}});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 7})},
                                             {1, Cart::Voice(2, 2, 60, {0, 0, 15, 1})},
                                             {2, Cart::Voice(3, wave, 60, {0, 0, 15, 4})},
                                             {3, Cart::Voice(4, 0, 60, {0, 0, 15, 2})},
                                             {4, Cart::Voice(1 | kVoiceFixed, 2, 60, {0, 0, 15, 7})},
                                             {5, Cart::Voice(1, 2, 60, {0, 0, 15, 0})},
                                             {6, Cart::Voice(1, 2, 60, {0, 0, 2, 1})},
                                             {7, Cart::Voice(0, sample, 60, {255, 0, 255, 250})},
                                             {8, Cart::Voice(kVoiceDrumKit, kit)},
                                             {9, cart.KeySplit(split, 59)}});

    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVol, 127);
    for (int v = 0; v < 8; v++)
    {
        t.Cmd(kCmdVoice, v).Note(12, 60, 100).Wait(24);
    }

    t.Cmd(kCmdVoice, 8).Note(12, 36, 100).Wait(24).Note(12, 38, 100).Wait(24).Note(12, 40, 100).Wait(24);
    t.Cmd(kCmdVoice, 9).Note(12, 50, 100).Wait(24).Note(12, 70, 100).Wait(24).Fine();
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();

    return {rom, Detect(rom)};
}

// With --psg-exclusive, each zone of a PSG voice whose release is slower than FluidSynth's fade for an exclusive class
// gets the class of its channel: 1 and 2 for the squares, 3 for the wave and 4 for the noise. The fixed-frequency bit
// doesn't change the channel. A PSG voice with a quicker release and a sample voice get no class. The MIDI file stays
// the same, and the SoundFont without the option has no class.
void TestPsgExclusive()
{
    const ExclusiveSong song = MakeExclusiveSong();
    ConvertSettings plain = TempSettings("exclusive_plain");
    ConvertSettings exclusive = TempSettings("exclusive");
    exclusive.psg_exclusive = true;

    const SongReport a = ConvertSong(song.rom, song.info, 0, plain, nullptr);
    const SongReport b = ConvertSong(song.rom, song.info, 0, exclusive, nullptr);

    SEQLIB_CHECK(a.ok && b.ok);
    SEQLIB_CHECK(ReadAll(a.midi_path) == ReadAll(b.midi_path));
    const auto without = ExclusiveClasses(ReadSf2(a.sf2_path));
    const auto with = ExclusiveClasses(ReadSf2(b.sf2_path));
    SEQLIB_REQUIRE_EQ(without.size(), 10);
    SEQLIB_REQUIRE_EQ(with.size(), 10);
    for (const auto& [program, zones] : without)
    {
        SEQLIB_CHECK(AllOfClass(zones, 0));
    }

    SEQLIB_CHECK(AllOfClass(with.at(0), 1));
    SEQLIB_CHECK(AllOfClass(with.at(1), 2));
    SEQLIB_CHECK(AllOfClass(with.at(2), 3));
    SEQLIB_CHECK(AllOfClass(with.at(3), 4));
    SEQLIB_CHECK(AllOfClass(with.at(4), 1));
    SEQLIB_CHECK(AllOfClass(with.at(5), 0));
    SEQLIB_CHECK(AllOfClass(with.at(6), 0));
    SEQLIB_CHECK(AllOfClass(with.at(7), 0));

    // The drum kit's slow noise on key 36 has the class, and its quick noise and its sample don't. The key split's
    // square 2 has class 2 on the keys up to 59, and its sample has none. The square's zones from key 36 tune runs of
    // keys to the driver's frequency settings.
    using Zones = std::vector<std::pair<int, int>>;
    SEQLIB_CHECK((with.at(8) == Zones{{0x2424, 4}, {0x2626, 0}, {0x2828, 0}}));
    const Zones& split = with.at(9);
    SEQLIB_REQUIRE(split.size() >= 3);
    SEQLIB_CHECK((split.front() == std::pair{0x2300, 2}));
    SEQLIB_CHECK((split.back() == std::pair{0x7F3C, 0}));
    int next_key = 36;
    for (size_t z = 1; z + 1 < split.size(); z++)
    {
        SEQLIB_CHECK_EQ(split[z].first & 0xFF, next_key);
        SEQLIB_CHECK_EQ(split[z].second, 2);
        next_key = (split[z].first >> 8) + 1;
    }

    SEQLIB_CHECK_EQ(next_key, 60);
}

// The exclusive classes go in a shared SoundFont too. They also go in the SoundFont of a conversion with a MIDI channel
// for each of the driver's sound channels. The option doesn't change that conversion's MIDI file either.
void TestPsgExclusiveShared()
{
    const ExclusiveSong song = MakeExclusiveSong();
    ConvertSettings exclusive = TempSettings("exclusive_shared");
    exclusive.psg_exclusive = true;
    ConvertSettings voices = TempSettings("exclusive_voices_plain");
    voices.voice_channels = true;
    ConvertSettings exclusive_voices = TempSettings("exclusive_voices");
    exclusive_voices.voice_channels = true;
    exclusive_voices.psg_exclusive = true;
    SoundfontBuilder shared(song.rom, song.info, true);
    const std::string shared_path = Utf8(TempPath("exclusive_shared.sf2"));
    std::string error;

    const SongReport s = ConvertSong(song.rom, song.info, 0, exclusive, &shared);
    const bool written = shared.File().Write(shared_path, error);
    const SongReport v = ConvertSong(song.rom, song.info, 0, voices, nullptr);
    const SongReport w = ConvertSong(song.rom, song.info, 0, exclusive_voices, nullptr);

    SEQLIB_CHECK(s.ok && written && v.ok && w.ok);
    SEQLIB_CHECK(ReadAll(v.midi_path) == ReadAll(w.midi_path));
    for (const std::string& path : {shared_path, w.sf2_path})
    {
        const auto classes = ExclusiveClasses(ReadSf2(path));
        SEQLIB_REQUIRE_EQ(classes.size(), 10);
        SEQLIB_CHECK(AllOfClass(classes.at(0), 1) && AllOfClass(classes.at(3), 4) && AllOfClass(classes.at(5), 0));
    }
}

// For each preset of bank 0, by program, returns one flag for each zone of the instrument that the preset plays, after
// the global zone. A flag is true if its zone contains a modulator from CC91 to the reverb send with an amount of 0.
std::map<int, std::vector<bool>> DryZones(const Sf2Records& sf)
{
    std::map<int, std::vector<bool>> out;
    ForBankZeroZones(
        sf,
        [&](int program, size_t z)
        {
            bool dry = false;
            for (size_t m = Le16(sf.Record("ibag", 4, z) + 2); m < Le16(sf.Record("ibag", 4, z + 1) + 2); m++)
            {
                const uint8_t* mod = sf.Record("imod", 10, m);
                dry =
                    dry || (Le16(mod) == (sf2::kController | 91) && Le16(mod + 2) == sf2::kReverbEffectsSend &&
                            Le16(mod + 4) == 0 && Le16(mod + 6) == sf2::kNoController && Le16(mod + 8) == sf2::kLinear);
            }

            out[program].push_back(dry);
        });

    return out;
}

// Every zone of a PSG voice contains kNoReverb: the default modulator from CC91 to the reverb send, at an amount of 0.
// A sample's zone keeps the default.
void TestPsgNoReverb()
{
    const ExclusiveSong song = MakeExclusiveSong();
    const SongReport r = ConvertSong(song.rom, song.info, 0, TempSettings("no_reverb"), nullptr);

    SEQLIB_CHECK(r.ok);
    const auto dry = DryZones(ReadSf2(r.sf2_path));
    SEQLIB_REQUIRE_EQ(dry.size(), 10);
    for (int program = 0; program < 7; program++)
    {
        const std::vector<bool>& zones = dry.at(program);
        SEQLIB_CHECK(!zones.empty() && std::all_of(zones.begin(), zones.end(), [](bool d) { return d; }));
    }

    SEQLIB_CHECK(!dry.at(7).empty() && std::none_of(dry.at(7).begin(), dry.at(7).end(), [](bool d) { return d; }));

    // In the drum kit, the zones of both noises are dry. The sample's zone isn't. In the key split, the zones of
    // square 2 cover the keys up to 59 and are dry. The sample's zone isn't.
    SEQLIB_CHECK((dry.at(8) == std::vector<bool>{true, true, false}));
    const std::vector<bool>& split = dry.at(9);
    SEQLIB_CHECK(split.size() >= 3 && std::all_of(split.begin(), split.end() - 1, [](bool d) { return d; }) &&
                 !split.back());
}

int RunTests()
{
    TestVoiceChannels();
    TestVoiceChannelsDrums();
    TestVoiceChannelsLoop();
    TestPsgExclusive();
    TestPsgExclusiveShared();
    TestPsgNoReverb();

    return seqlib::testing::Summary("mp2k_voice_channels");
}

} // namespace
} // namespace agblib::sequence_export::mp2k

int main()
{
    return agblib::sequence_export::mp2k::RunTests();
}
