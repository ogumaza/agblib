// SPDX-License-Identifier: MIT

// Unit tests for MP2K's conversion: the MIDI and SoundFont files, the performance for an editor, the levels and the
// drum channel. mp2k_voice_channels_test.cpp tests the conversion with a channel for each voice, and the PSG's zones.
// They build small synthetic cartridges in memory. No game ROM is needed.

#include "agb_conversion_test_util.h"
#include "mp2k_test_util.h"

namespace agblib::sequence_export::mp2k
{
namespace
{

using agblib::testing::agb_tests::CheckFilesInMemory;

// Returns the bank and program that each program change on `channel` selects: the bank of the last bank select before
// it in its track.
std::vector<std::pair<int, int>> Selections(const MidiEvents& m, int channel)
{
    std::vector<std::pair<int, int>> out;
    for (const auto& t : m.tracks)
    {
        int bank = 0;
        for (const auto& e : t.events)
        {
            if (e.bytes[0] == 0xB0 + channel && e.bytes[1] == cc::kBankSelect)
            {
                bank = e.bytes[2];
            }
            else if (e.bytes[0] == 0xC0 + channel)
            {
                out.push_back({bank, e.bytes[1]});
            }
        }
    }

    return out;
}
// Returns the CC10 and CC11 events of the MIDI track named after track `t`, in order. Each is a tick, a controller and
// a value.
std::vector<std::tuple<uint32_t, int, int>> LevelEvents(const MidiEvents& m, int t)
{
    const std::string name = "Track " + std::to_string(t);
    std::vector<std::tuple<uint32_t, int, int>> out;
    for (const auto& track : m.tracks)
    {
        if (track.events.empty())
        {
            continue;
        }

        const std::vector<uint8_t>& first = track.events.front().bytes;
        if (first.size() < 3 || std::string(first.begin() + 3, first.end()) != name)
        {
            continue;
        }

        for (const auto& e : track.events)
        {
            if ((e.bytes[0] & 0xF0) == 0xB0 && (e.bytes[1] == cc::kPan || e.bytes[1] == cc::kExpression))
            {
                out.emplace_back(uint32_t(e.tick), e.bytes[1], e.bytes[2]);
            }
        }
    }

    return out;
}
void TestConversion()
{
    // A sample voice with a bend, a key split of a sample and a square wave, and a drum kit. The kit has a fixed-pitch
    // sample panned left and three noise voices: one with no pan, one panned right and one panned a little left.
    Cart cart;
    const uint32_t lead = cart.Wave(Saw(), 11025, 32);
    const uint32_t low = cart.Wave(Saw(100), 8000);
    const uint32_t kick = cart.Wave(Saw(50), 13379);
    const uint32_t split_table = cart.VoiceGroup({{0, Cart::Voice(0, low)}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    const uint32_t kit_table = cart.VoiceGroup({{36, Cart::Voice(kFixed, kick, 50, {255, 0, 255, 0}, 0xA0)},
                                                {38, Cart::Voice(4, 0, 40, {0, 2, 0, 0})},
                                                {40, Cart::Voice(4, 0, 40, {0, 2, 0, 0}, 0xD8)},
                                                {41, Cart::Voice(4, 0, 40, {0, 2, 0, 0}, 0xB0)}});
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(0, lead, 60, {255, 200, 128, 200})},
                                             {2, cart.KeySplit(split_table, 59)},
                                             {3, Cart::Voice(kVoiceDrumKit, kit_table)}});

    // The drum kit's track is track 9, after seven that only end.
    std::vector<Track> tracks = {
        Track()
            .Cmd(kCmdTempo, 75)
            .Cmd(kCmdVoice, 1)
            .Cmd(kCmdVol, 100)
            .Cmd(kCmdPan, 0x50)
            .Cmd(kCmdBendR, 12)
            .Cmd(kCmdBend, 0x50)
            .Note(24, 60, 100)
            .Wait(48)
            .Fine(),
        Track().Cmd(kCmdVoice, 2).Cmd(kCmdVol, 127).Note(24, 50, 90).Note(24, 70, 90).Wait(48).Fine()};
    tracks.resize(9, Track().Fine());
    tracks.push_back(
        Track().Cmd(kCmdVoice, 3).Cmd(kCmdVol, 127).Note(12, 36, 127).Wait(12).Note(12, 38, 127).Wait(36).Fine());
    cart.Song(tracks, voices, 0, 0, 0x80 | 40);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("convert");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK_EQ(r.tracks, 3);
    SEQLIB_CHECK(std::fabs(r.bpm - 60e6 / 401825) < 1e-9);
    SEQLIB_CHECK(PathFromUtf8(r.midi_path).filename() == "convert_00.mid");

    // 960 ticks a quarter note, the tempo at the GBA's speed, and a conductor track, then a track on the MIDI channel
    // of each of the song's tracks that plays notes. Track 9 moves off the drum channel to channel 11, the first free
    // one after it.
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK_EQ(m.division, 24 * kMidiTicks);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 4);
    const std::vector<seqlib::midi::Tempo> tempos = seqlib::midi::Tempos(m);
    SEQLIB_CHECK(std::any_of(tempos.begin(), tempos.end(), [](const seqlib::midi::Tempo& t)
                             { return t.track == 0 && t.micros_per_quarter == 401825; }));

    // A velocity of v is written as 127 × √(v / 127), rounded: the lead's 100 as 113.
    const auto lead_notes = m.Find(0x90);
    SEQLIB_CHECK(lead_notes.size() == 1 && lead_notes[0].tick == 0 && lead_notes[0].bytes[1] == 60 &&
                 lead_notes[0].bytes[2] == 113);
    const auto lead_offs = m.Find(0x80);
    SEQLIB_CHECK(lead_offs.size() == 1 && lead_offs[0].tick == 24 * kMidiTicks);
    SEQLIB_REQUIRE_EQ(m.Find(0x91).size(), 2);
    SEQLIB_REQUIRE_EQ(m.Find(0x9A).size(), 2);
    SEQLIB_CHECK(m.Find(0x99).empty());

    // The program change, full volume and the song's reverb. The track's volume of 100 and pan of 0x50 give it levels
    // of 74 on the left and 125 on the right, out of 128. CC10 84 and CC11 114 give those levels under a constant-power
    // pan law and the SF2 default curve of CC11. A bend range of 3 semitones fills the bend of 3 semitones.
    const auto programs = m.Find(0xC0);
    SEQLIB_CHECK(programs.size() == 1 && programs[0].bytes[1] == 1);
    std::map<int, int> controls;
    for (const auto& e : m.Find(0xB0))
    {
        controls[e.bytes[1]] = e.bytes[2];
    }

    SEQLIB_CHECK(controls[cc::kVolume] == 127 && controls[cc::kReverb] == 40);
    SEQLIB_CHECK(controls[cc::kPan] == 84 && controls[cc::kExpression] == 114);
    SEQLIB_CHECK_EQ(controls[cc::kDataEntry], 3);
    const auto bends = m.Find(0xE0);
    SEQLIB_CHECK(!bends.empty() && bends.back().bytes[1] == 0x7F && bends.back().bytes[2] == 0x7F);

    // The SoundFont: a preset in bank 0 for each voice. No note plays on the drum channel. Bank 128 has no presets.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    std::map<std::pair<int, int>, int> presets;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        presets[{Le16(h + 22), Le16(h + 20)}] = int(RecordedInstrument(sf, p));
    }

    SEQLIB_CHECK(presets.count({0, 1}) && presets.count({0, 2}) && presets.count({0, 3}));
    SEQLIB_REQUIRE_EQ(presets.size(), 3);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf));

    auto zone_of = [&](int instrument, int index)
    {
        return sf.ZoneGens(Le16(sf.Record("inst", 22, size_t(instrument)) + 20) + size_t(index));
    };

    // The lead's zone: its envelope, and no tuning, since its sample's rate is a whole number. Its sample contains the
    // master volume of 12, at 13/16 of the saw's -128 widened to 16 bits, rather than the zone's attenuation.
    const auto lead_zone = zone_of(presets[{0, 1}], 1);
    SEQLIB_CHECK(lead_zone.count(sf2::kDecayVolEnv) && lead_zone.count(sf2::kSustainVolEnv) &&
                 lead_zone.count(sf2::kReleaseVolEnv) && !lead_zone.count(sf2::kAttackVolEnv));
    SEQLIB_CHECK(!lead_zone.count(sf2::kInitialAttenuation));
    SEQLIB_CHECK_EQ(sf.SamplePeak(lead_zone.at(sf2::kSampleId)), 128 * 256 * 13 / 16);
    SEQLIB_CHECK(!lead_zone.count(sf2::kCoarseTune) && !lead_zone.count(sf2::kFineTune));
    SEQLIB_CHECK_EQ(lead_zone.at(sf2::kSampleModes), 1);

    // The key split's zones split after key 59, and its square's sample is 6.3 dB quieter than a full-scale sample.
    const auto square_zone = zone_of(presets[{0, 2}], 2);
    SEQLIB_CHECK_EQ(zone_of(presets[{0, 2}], 1).at(sf2::kKeyRange), 0x3B00);
    SEQLIB_CHECK_EQ(square_zone.at(sf2::kKeyRange) & 0xFF, 60);
    SEQLIB_CHECK(!square_zone.count(sf2::kInitialAttenuation));
    SEQLIB_CHECK(std::fabs(20 * std::log10(sf.SamplePeak(square_zone.at(sf2::kSampleId)) / 32767.0) + 6.3) < 0.01);

    // The square's zones follow one another up to key 127. Each tunes its keys to the driver's frequency settings. On
    // key 96, the driver shifts C's entry of -2004 right 5 times and adds 2048. The setting of 1985 plays 2080.5 Hz, 10
    // cents below C7 in equal temperament.
    const size_t split_zones = Le16(sf.Record("inst", 22, size_t(presets[{0, 2}]) + 1) + 20) -
                               Le16(sf.Record("inst", 22, size_t(presets[{0, 2}])) + 20);
    int next_key = 60, key_96_cents = 0;
    for (size_t z = 2; z < split_zones; z++)
    {
        const auto zone = zone_of(presets[{0, 2}], int(z));
        const int lo = zone.at(sf2::kKeyRange) & 0xFF, hi = zone.at(sf2::kKeyRange) >> 8;
        SEQLIB_CHECK_EQ(lo, next_key);
        next_key = hi + 1;
        if (lo <= 96 && hi >= 96)
        {
            SEQLIB_CHECK(!zone.count(sf2::kCoarseTune));
            key_96_cents = zone.count(sf2::kFineTune) ? int16_t(zone.at(sf2::kFineTune)) : 0;
        }
    }

    SEQLIB_CHECK_EQ(next_key, 128);
    SEQLIB_CHECK_EQ(key_96_cents, -10);

    // The drum kit's fixed-pitch sample plays at the mixer's rate whatever the key or the bend. Its pan of 0xA0 gives
    // it 191/127 of the left level of a note with no pan and 64/128 of the right level. Pan -296 gives the sides that
    // ratio under a sin/cos pan law. The noise voice with no pan plays the noise setting of its key. Its zone has no
    // pan.
    const auto kick_zone = zone_of(presets[{0, 3}], 1);
    SEQLIB_CHECK_EQ(kick_zone.at(sf2::kKeyRange), 0x2424);
    SEQLIB_CHECK_EQ(kick_zone.at(sf2::kScaleTuning), 0);
    SEQLIB_CHECK_EQ(int16_t(kick_zone.at(sf2::kPan)), -296);
    const auto noise_zone = zone_of(presets[{0, 3}], 2);
    SEQLIB_CHECK_EQ(noise_zone.at(sf2::kKeyRange), 0x2626);
    SEQLIB_CHECK_EQ(noise_zone.at(sf2::kScaleTuning), 0);
    SEQLIB_CHECK(!noise_zone.count(sf2::kPan));

    // The PSG plays a note on one side alone where that side's level is at least twice the other's. It plays in the
    // middle otherwise. The pan of 0xD8 gives 176 on the right and 79 on the left. The pan of 0xB0 gives 96 on the
    // right and 159 on the left. On one side alone, the zone's attenuation cancels the 3 dB that the RP-036 pan law
    // adds there. Most players apply 0.4 of a zone's attenuation. The zone therefore has 75 cB.
    const auto right_zone = zone_of(presets[{0, 3}], 3), middle_zone = zone_of(presets[{0, 3}], 4);
    SEQLIB_CHECK(right_zone.at(sf2::kKeyRange) == 0x2828 && middle_zone.at(sf2::kKeyRange) == 0x2929);
    SEQLIB_CHECK(right_zone.count(sf2::kPan) && int16_t(right_zone.at(sf2::kPan)) == 500);
    SEQLIB_CHECK(right_zone.count(sf2::kInitialAttenuation) && right_zone.at(sf2::kInitialAttenuation) == 75);
    SEQLIB_CHECK(!middle_zone.count(sf2::kPan) && !middle_zone.count(sf2::kInitialAttenuation));
    SEQLIB_CHECK(!noise_zone.count(sf2::kInitialAttenuation));
    bool wheel_off = false;
    for (size_t i = 0; i < sf.Count("imod", 10); i++)
    {
        const uint8_t* mod = sf.Record("imod", 10, i);
        wheel_off = wheel_off || (Le16(mod) == 0x020E && Le16(mod + 2) == sf2::kFineTune && Le16(mod + 4) == 0);
    }

    SEQLIB_CHECK(wheel_off);
}

void TestSongFiles()
{
    // ConvertSongFiles gives the bytes of the files that ConvertSong writes, and names no paths. A song whose music
    // player plays no track gives no bytes.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Note(12, 64, 90).Wait(24);
    t.Fine();
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const ConvertSettings opt = TempSettings("song_files");

    const SongReport written = ConvertSong(rom, info, 0, opt, nullptr);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);
    const SongFiles none = ConvertSongFiles(rom, info, 1, opt);

    CheckFilesInMemory(written, files);
    SEQLIB_CHECK(!none.report.ok && none.midi.empty() && none.sf2.empty());
}

void TestVoiceGroupFiles()
{
    // A voice group's SoundFont contains the instruments of the SoundFont of a song that plays the same voices, byte
    // for byte with the song's title. Voice 2 has no sample: it gets a warning and no preset. Without a preset, the
    // SoundFont stays empty.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(2, 2, 60, {0, 0, 15, 0})},
                                             {1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))},
                                             {2, Cart::Voice(0, 0)}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Cmd(kCmdVoice, 1);
    t.Note(24, 64, 90).Wait(24).Fine();
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const ConvertSettings opt = TempSettings("voice_group");
    const SongFiles song = ConvertSongFiles(rom, info, 0, opt);
    const std::string title = rom.Title() + " #" + agblib::sequence_export::agb::SongNumber(0, info.song_count);

    const VoiceGroupFiles group = ConvertVoiceGroup(rom, info, voices, {0, 1, 2}, opt, title);
    const VoiceGroupFiles empty = ConvertVoiceGroup(rom, info, voices, {2, 200}, opt, title);

    SEQLIB_REQUIRE(song.report.ok && group.ok);
    SEQLIB_CHECK(!group.sf2.empty() && group.sf2 == song.sf2);
    SEQLIB_CHECK(group.warnings == std::vector<std::string>{"voice 2 has no samples to play"});
    SEQLIB_CHECK(!empty.ok && empty.sf2.empty() && empty.warnings.size() == 1);
}

void TestNoteChannels()
{
    // Voice 0 plays the square wave of PSG channel 2, and voice 1 a sample. Track 0's second square note takes the
    // channel while the driver still plays the first, and cuts that one short. Track 1's square note meets the channel
    // that an earlier track plays on at the same priority, and never starts. Its sample note plays on a DirectSound
    // channel.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(2, 2, 60, {0, 0, 15, 0})}, {1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(48, 60, 100).Wait(24).Note(24, 62, 100).Wait(24);
    first.Fine();
    Track second;
    second.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12);
    const size_t dropped = second.Here();
    second.Note(12, 64, 90).Wait(12).Cmd(kCmdVoice, 1).Note(12, 67, 90).Wait(12).Fine();
    cart.Song({first, second}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    agblib::mp2k::SongHeader header;
    SEQLIB_REQUIRE(ReadSong(rom, info, 0, header) && header.tracks.size() == 2);

    const Performance perf = PerformSong(rom, info, 0, TempSettings("note_channels"));

    constexpr int8_t kSquare = kFirstPsgChannel + 1;
    SEQLIB_REQUIRE_EQ(perf.note_commands.size(), 3);
    SEQLIB_REQUIRE_EQ(perf.note_channels.size(), 3);
    SEQLIB_CHECK(perf.note_channels[0] == (NoteChannel{kSquare, 2, 24}));
    SEQLIB_CHECK(perf.note_channels[1] == (NoteChannel{kSquare, 2, std::nullopt}));
    SEQLIB_CHECK(perf.note_channels[2].channel >= 0 && perf.note_channels[2].channel < kSquare - 1);
    SEQLIB_CHECK_EQ(perf.note_channels[2].psg, 0);
    SEQLIB_CHECK(!perf.note_channels[2].cut_tick);
    const uint32_t offset = header.tracks[1] + uint32_t(dropped) - perf.base;
    SEQLIB_REQUIRE_EQ(perf.unstarted.size(), 1);
    SEQLIB_CHECK(perf.unstarted[0] == (UnstartedNote{12, 12, 1, 64, 90, 2, offset}));

    // The MIDI file's notes stay those of a performance without the records: the cut note ends where the next took its
    // channel.
    uint32_t end = 0;
    for (const ntdlib::sequence_export::TrackEvent& event : perf.tracks[0])
    {
        if (event.kind == ntdlib::sequence_export::EventKind::kNoteOff && event.note == 1)
        {
            end = event.tick;
        }
    }

    SEQLIB_CHECK_EQ(end, 24);
}

void TestPsgNoteTakenBeforeItSounds()
{
    // Voice 0 plays the square wave of PSG channel 2 with the slowest attack. The channel starts the first note at a
    // level of 0. The second note takes the channel a tick later, before the level rises, and the first note never
    // sounds. The performance lists it among the notes that never started, and the MIDI file leaves it out.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(2, 2, 60, {7, 0, 15, 0})}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t taken = t.Here();
    t.Note(24, 60, 100).Wait(1).Note(24, 62, 100).Wait(24).Fine();
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    agblib::mp2k::SongHeader header;
    SEQLIB_REQUIRE(ReadSong(rom, info, 0, header) && header.tracks.size() == 1);

    const Performance perf = PerformSong(rom, info, 0, TempSettings("psg_taken"));

    constexpr int8_t kSquare = kFirstPsgChannel + 1;
    SEQLIB_REQUIRE_EQ(perf.note_channels.size(), 1);
    SEQLIB_CHECK(perf.note_channels[0] == (NoteChannel{kSquare, 2, std::nullopt}));
    const uint32_t offset = header.tracks[0] + uint32_t(taken) - perf.base;
    SEQLIB_REQUIRE_EQ(perf.unstarted.size(), 1);
    SEQLIB_CHECK(perf.unstarted[0] == (UnstartedNote{0, 24, 0, 60, 100, 2, offset}));
    std::vector<int> keys;
    for (const ntdlib::sequence_export::TrackEvent& event : perf.tracks[0])
    {
        if (event.kind == ntdlib::sequence_export::EventKind::kNoteOn)
        {
            keys.push_back(event.key);
        }
    }

    SEQLIB_CHECK(keys == std::vector<int>{62});
}

void TestPerformance()
{
    // Track 0 sets the tempo, the voice, the volume and the pan, plays a note, and calls a pattern with a second note.
    // Track 1 plays a note, and its repeat goes back to the note once.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x50).Note(24, 60, 100).Wait(24);
    const size_t call = first.Here();
    first.Patt(19).Wait(24).Fine();
    SEQLIB_REQUIRE_EQ(first.Here(), 19);
    first.Note(12, 62).Wait(12).Raw({kCmdPend});
    Track second;
    second.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127);
    const size_t note = second.Here();
    second.Note(12, 64, 90).Wait(12).Rept(2, note).Fine();
    cart.Song({first, second}, voices);

    // Song 1 loops on its note. Its GOTO runs in the end's tick, and so does the note of the next pass.
    Track looping;
    looping.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127);
    const size_t loop = looping.Here();
    looping.Note(24, 60, 100).Wait(24).Goto(loop);
    cart.Song({looping}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const Performance perf = PerformSong(rom, info, 0, TempSettings("performance"));

    // The Cart puts each track at a multiple of 4 bytes: track 1 starts 24 bytes after track 0. A tempo of 75 takes a
    // tick each frame, at the GBA's 16,777,216 / 280,896 frames a second.
    constexpr uint32_t kSecond = 24;
    SEQLIB_CHECK(perf.warnings.empty());
    SEQLIB_CHECK_EQ(perf.timebase, 24);
    SEQLIB_CHECK_EQ(perf.end_tick, 60);
    SEQLIB_CHECK(!perf.loop);
    SEQLIB_REQUIRE_EQ(perf.tempo.size(), 1);
    SEQLIB_CHECK_EQ(perf.tempo[0].tick, 0);
    SEQLIB_CHECK(std::fabs(perf.tempo[0].bpm - 150.0 * (16777216.0 / 280896) / 60) < 1e-9);

    // The stretches, with offsets from track 0's start.
    using Entry = ntdlib::sequence_export::Entry;
    const std::vector<ntdlib::sequence_export::Stretch> track0 = {{0, 0, uint32_t(call), Entry::kOpen, 0, false},
                                                                  {24, 19, 22, Entry::kCall, 1, false},
                                                                  {36, 17, 18, Entry::kReturn, 0, false}};
    const std::vector<ntdlib::sequence_export::Stretch> track1 = {
        {0, kSecond, kSecond + 8, Entry::kOpen, 0, false}, {12, kSecond + 4, kSecond + 14, Entry::kLoop, 0, false}};
    SEQLIB_CHECK(perf.stretches[0] == track0);
    SEQLIB_CHECK(perf.stretches[1] == track1);
    for (size_t t = 2; t < perf.stretches.size(); t++)
    {
        SEQLIB_CHECK(perf.stretches[t].empty() && perf.tracks[t].empty() && perf.mix[t].empty());
    }

    // The notes, numbered in the order that they start, and the commands that started them.
    const std::vector<ntdlib::sequence_export::NoteCommand> commands = {
        {8, 0}, {kSecond + 4, 0}, {kSecond + 4, 1}, {19, 1}};
    SEQLIB_CHECK(perf.note_commands == commands);
    const auto event =
        [](uint32_t tick, ntdlib::sequence_export::EventKind kind, uint8_t key, uint8_t value, uint32_t id)
    {
        ntdlib::sequence_export::TrackEvent e;
        e.tick = tick;
        e.kind = kind;
        e.key = key;
        e.value = value;
        e.note = id;
        return e;
    };
    ntdlib::sequence_export::TrackEvent preset;
    preset.kind = ntdlib::sequence_export::EventKind::kPreset;
    preset.preset = 0;
    const std::vector<ntdlib::sequence_export::TrackEvent> events0 = {
        preset, event(0, ntdlib::sequence_export::EventKind::kNoteOn, 60, 100, 1),
        event(24, ntdlib::sequence_export::EventKind::kNoteOff, 60, 0, 1),
        event(24, ntdlib::sequence_export::EventKind::kNoteOn, 62, 100, 4),
        event(36, ntdlib::sequence_export::EventKind::kNoteOff, 62, 0, 4)};
    const std::vector<ntdlib::sequence_export::TrackEvent> events1 = {
        preset, event(0, ntdlib::sequence_export::EventKind::kNoteOn, 64, 90, 2),
        event(12, ntdlib::sequence_export::EventKind::kNoteOff, 64, 0, 2),
        event(12, ntdlib::sequence_export::EventKind::kNoteOn, 64, 90, 3),
        event(24, ntdlib::sequence_export::EventKind::kNoteOff, 64, 0, 3)};
    const auto same = [](const std::vector<ntdlib::sequence_export::TrackEvent>& a,
                         const std::vector<ntdlib::sequence_export::TrackEvent>& b)
    {
        const auto fields = [](const ntdlib::sequence_export::TrackEvent& e)
        {
            return std::tuple(e.tick, e.kind, e.key, e.value, e.preset, e.note);
        };
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [&](const ntdlib::sequence_export::TrackEvent& x,
                              const ntdlib::sequence_export::TrackEvent& y) { return fields(x) == fields(y); });
    };
    SEQLIB_CHECK(same(perf.tracks[0], events0));
    SEQLIB_CHECK(same(perf.tracks[1], events1));

    // The volume and the pan less 0x40.
    const std::vector<ntdlib::sequence_export::MixChange> mix0 = {{0, ntdlib::sequence_export::Mix::kVolume, 100},
                                                                  {0, ntdlib::sequence_export::Mix::kPan, 16}};
    const std::vector<ntdlib::sequence_export::MixChange> mix1 = {{0, ntdlib::sequence_export::Mix::kVolume, 127}};
    SEQLIB_CHECK(perf.mix[0] == mix0);
    SEQLIB_CHECK(perf.mix[1] == mix1);

    // One pass of song 1's loop. The GOTO finishes the first stretch in the end's tick. The next pass's stretch begins
    // there, and stays out.
    ConvertSettings once = TempSettings("performance_loop");
    once.loops = 1;
    const Performance loops = PerformSong(rom, info, 1, once);
    SEQLIB_REQUIRE(loops.loop.has_value());
    SEQLIB_CHECK(loops.loop->start == 0 && loops.loop->end == 24);
    SEQLIB_CHECK_EQ(loops.end_tick, 24);
    const std::vector<ntdlib::sequence_export::Stretch> pass = {{0, 0, 8, Entry::kOpen, 0, false}};
    SEQLIB_CHECK(loops.stretches[0] == pass);
    const std::vector<ntdlib::sequence_export::NoteCommand> note_commands = {{4, 0}};
    SEQLIB_CHECK(loops.note_commands == note_commands);
}

void TestLevels()
{
    // A sample voice, and a voice for each square channel.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))},
                                             {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})},
                                             {2, Cart::Voice(2, 2, 60, {0, 0, 15, 0})}});

    // Tracks 0 to 2 play the sample voice: at volume 80 and pan 0x7F, at volume 127 and pan 0x7F, and at volume 100
    // and pan 0x50. Tracks 3 to 5 play square 1 at volume 100 in turn: at pan 0x50 from tick 0, 0x60 from tick 24 and
    // 0x20 from tick 48. Track 6 plays square 2, the sample voice and square 2 again from ticks 0, 24 and 48, at volume
    // 100 and pan 0x60.
    std::vector<Track> tracks;
    const std::vector<std::pair<int, int>> kSamples = {{80, 0x7F}, {127, 0x7F}, {100, 0x50}};
    for (const auto& [volume, pan] : kSamples)
    {
        tracks.push_back(
            Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, volume).Cmd(kCmdPan, pan).Note(12, 60, 127).Wait(12).Fine());
    }

    tracks.push_back(Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x50).Note(12, 60, 127).Wait(12).Fine());
    tracks.push_back(
        Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x60).Wait(24).Note(12, 60, 127).Wait(12).Fine());
    tracks.push_back(
        Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x20).Wait(48).Note(12, 60, 127).Wait(12).Fine());
    tracks.push_back(Track()
                         .Cmd(kCmdVol, 100)
                         .Cmd(kCmdPan, 0x60)
                         .Cmd(kCmdVoice, 2)
                         .Note(12, 60, 127)
                         .Wait(24)
                         .Cmd(kCmdVoice, 0)
                         .Note(12, 60, 127)
                         .Wait(24)
                         .Cmd(kCmdVoice, 2)
                         .Note(12, 60, 127)
                         .Wait(12)
                         .Fine());
    cart.Song(tracks, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("levels");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // A sample plays at the levels that the volume and pan give each side, out of 128. Volume 80 at pan 0x7F gives 0
    // and 158. Volume 127 gives 0 and 252. CC11 can't reach that: it stops at the centre's loudest level. Volume 100 at
    // pan 0x50 gives 74 and 125.
    using Levels = std::vector<std::tuple<uint32_t, int, int>>;
    SEQLIB_REQUIRE(r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK((LevelEvents(m, 0) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 119}}));
    SEQLIB_CHECK((LevelEvents(m, 1) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 127}}));
    SEQLIB_CHECK((LevelEvents(m, 2) == Levels{{0, cc::kPan, 84}, {0, cc::kExpression, 114}}));

    // A square plays on one side alone where that side's level is at least twice the other's. That side plays at the
    // level of the centre: the average of the two. Otherwise the square plays on both sides at that level. Pan 0x50
    // gives 74 and 125. The square then plays on both sides at 99.5. Pan 0x60 gives 49 and 150, on the right alone.
    // Pan 0x20 gives 149 and 50, on the left alone. A track's controllers follow the PSG's rule from the start where
    // its first note is a PSG note.
    SEQLIB_CHECK((LevelEvents(m, 3) == Levels{{0, cc::kPan, 64}, {0, cc::kExpression, 112}}));
    SEQLIB_CHECK((LevelEvents(m, 4) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 95}}));
    SEQLIB_CHECK((LevelEvents(m, 5) == Levels{{0, cc::kPan, 0}, {0, cc::kExpression, 95}}));

    // A track that plays both kinds of voice follows the rule of the notes that it plays.
    SEQLIB_CHECK((LevelEvents(m, 6) == Levels{{0, cc::kPan, 127},
                                              {0, cc::kExpression, 95},
                                              {24 * kMidiTicks, cc::kPan, 101},
                                              {24 * kMidiTicks, cc::kExpression, 119},
                                              {48 * kMidiTicks, cc::kPan, 127},
                                              {48 * kMidiTicks, cc::kExpression, 95}}));
}

void TestMonoLevels()
{
    // A cartridge with the mono mixer, a sample voice and a square voice.
    Cart cart;
    PutMonoMixer(cart);
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});

    // Track 0 plays the sample voice at volume 80 and pan 0x7F, and track 1 the square at volume 100 and pan 0x60.
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 80).Cmd(kCmdPan, 0x7F).Note(12, 60, 127).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x60).Note(12, 60, 127).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("mono_levels");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The mixer plays the sample on both sides at the average of its levels, 0 and 158. The PSG still plays the square
    // on the right alone.
    using Levels = std::vector<std::tuple<uint32_t, int, int>>;
    SEQLIB_REQUIRE(info.mono && r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK((LevelEvents(m, 0) == Levels{{0, cc::kPan, 64}, {0, cc::kExpression, 100}}));
    SEQLIB_CHECK((LevelEvents(m, 1) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 95}}));
}

void TestSlowAttackAtEnd()
{
    // A square note with the slowest attack starts on the last tick of each pass through a loop of 23 ticks from tick
    // 1, and the conversion ends after the second pass, on tick 47. The second pass's note goes on into the next pass,
    // and its level gets above 0 only after the end. So the model runs on past the end, and the MIDI file keeps it.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {7, 0, 15, 0})}});
    Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Wait(1);
    const size_t loop = track.Here();
    track.Wait(22).Note(24, 60, 100).Wait(1).Goto(loop);
    cart.Song({track}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("slow_attack");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok);
    std::vector<uint32_t> ons;
    for (const auto& e : ReadMidi(r.midi_path).Find(0x90))
    {
        ons.push_back(uint32_t(e.tick));
    }

    SEQLIB_CHECK(ons == (std::vector<uint32_t>{23 * kMidiTicks, 46 * kMidiTicks}));
}

void TestTwoVoicesInATick()
{
    // A track sets voice 1 and plays a note, then sets voice 4 and plays another, in one tick. The driver plays each
    // note with the voice set before it. So the MIDI file gives the program changes and notes in the track's order.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup(
        {{1, Cart::Voice(0, cart.Wave(Saw(80), 13379, 0))}, {4, Cart::Voice(0, cart.Wave(Saw(90), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 100)
                   .Note(24, 60, 100)
                   .Cmd(kCmdVoice, 4)
                   .Note(24, 64, 100)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("two_voices");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok);
    std::vector<std::pair<int, int>> order;
    for (const auto& t : ReadMidi(r.midi_path).tracks)
    {
        for (const auto& [tick, bytes] : t.events)
        {
            if (tick == 0 && (bytes[0] == 0xC0 || bytes[0] == 0x90))
            {
                order.emplace_back(bytes[0], bytes[1]);
            }
        }
    }

    const std::vector<std::pair<int, int>> kOrder = {{0xC0, 1}, {0x90, 60}, {0xC0, 4}, {0x90, 64}};
    SEQLIB_CHECK(order == kOrder);
}

void TestSharedSoundfont()
{
    // Two voice groups with different voices as program 1 and the same voice as program 4.
    Cart cart;
    const std::vector<uint8_t> common = Cart::Voice(0, cart.Wave(Saw(), 13379, 0));
    const uint32_t first_group = cart.VoiceGroup({{1, Cart::Voice(0, cart.Wave(Saw(80)))}, {4, common}});
    const uint32_t second_group = cart.VoiceGroup({{1, Cart::Voice(0, cart.Wave(Saw(90)))}, {4, common}});

    // The first song plays program 1 on track 0 and program 4 on track 9, and the second plays both on track 0.
    std::vector<Track> tracks = {Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine()};
    tracks.resize(9, Track().Fine());
    tracks.push_back(Track().Cmd(kCmdVoice, 4).Cmd(kCmdVol, 100).Note(24, 36, 100).Wait(24).Fine());
    cart.Song(tracks, first_group);
    cart.Song({Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 100)
                   .Note(24, 60, 100)
                   .Wait(24)
                   .Cmd(kCmdVoice, 4)
                   .Note(24, 62, 100)
                   .Wait(24)
                   .Fine()},
              second_group);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("shared");
    SoundfontBuilder shared(rom, info);

    const SongReport first = ConvertSong(rom, info, 0, opt, &shared);
    const SongReport second = ConvertSong(rom, info, 1, opt, &shared);

    // Program 1 has the first song's voice in bank 0 and the second's in bank 1, the common voice has one preset, and
    // the drum bank has none.
    SEQLIB_CHECK(first.ok && second.ok && first.sf2_path.empty() && second.sf2_path.empty());
    std::map<std::pair<int, int>, int> presets;
    for (const sf2::Preset& p : shared.File().presets)
    {
        presets[{p.bank, p.program}] = PresetInstrument(p);
    }

    SEQLIB_REQUIRE_EQ(presets.size(), 3);
    SEQLIB_CHECK(presets.count({0, 1}) && presets.count({1, 1}) && presets.count({0, 4}));
    SEQLIB_CHECK((presets[{0, 1}] != presets[{1, 1}]));

    // Each program change selects its preset's bank, and track 9 plays on channel 11, away from the drum channel.
    const MidiEvents a = ReadMidi(first.midi_path);
    const MidiEvents b = ReadMidi(second.midi_path);
    using Slots = std::vector<std::pair<int, int>>;
    SEQLIB_CHECK(a.Find(0x99).empty() && a.Find(0x9A).size() == 1);
    SEQLIB_CHECK(Selections(a, 0) == (Slots{{0, 1}}));
    SEQLIB_CHECK(Selections(a, 10) == (Slots{{0, 4}}));
    SEQLIB_CHECK(Selections(b, 0) == (Slots{{1, 1}, {0, 4}}));
}

void TestSharedPresets()
{
    // 129 instruments that songs play as program 5, and then another as program 0. The 129 fill the 128 banks of
    // program 5. SharedPreset() doesn't look at the instruments, the ROM or the driver. They therefore don't have to
    // exist.
    const Rom rom = Cart().ToRom();
    const DriverInfo info;
    SoundfontBuilder sf(rom, info);
    std::vector<BankProgram> slots;

    for (int i = 0; i < 129; i++)
    {
        slots.push_back(sf.SharedPreset(5, i));
    }

    const BankProgram again = sf.SharedPreset(5, 128);
    const BankProgram next = sf.SharedPreset(0, 200);

    // The 129th gets the first free program of bank 0 and keeps it. The next one as program 0 gets bank 1.
    SEQLIB_CHECK(slots[0].bank == 0 && slots[0].program == 5);
    SEQLIB_CHECK(slots[127].bank == 127 && slots[127].program == 5);
    SEQLIB_CHECK(slots[128].bank == 0 && slots[128].program == 0);
    SEQLIB_CHECK(again.bank == 0 && again.program == 0);
    SEQLIB_CHECK(next.bank == 1 && next.program == 0);
    SEQLIB_REQUIRE_EQ(sf.File().presets.size(), 130);
}

void TestSharedDrumChannel()
{
    // Two songs whose 16 tracks all play, one after another, with different voices as program 2. Track 9 therefore has
    // no free channel to move to.
    Cart cart(kMode, {16});
    const uint32_t first_group = cart.VoiceGroup({{2, Cart::Voice(0, cart.Wave(Saw(70)))}});
    const uint32_t second_group = cart.VoiceGroup({{2, Cart::Voice(0, cart.Wave(Saw(90)))}});
    std::vector<Track> tracks;
    for (int t = 0; t < 16; t++)
    {
        tracks.push_back(Track().Wait(24 * t).Cmd(kCmdVoice, 2).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine());
    }

    cart.Song(tracks, first_group);
    cart.Song(tracks, second_group);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("drums");
    SoundfontBuilder shared(rom, info);

    const SongReport first = ConvertSong(rom, info, 0, opt, &shared);
    const SongReport second = ConvertSong(rom, info, 1, opt, &shared);

    auto warns = [](const SongReport& s)
    {
        return std::any_of(s.warnings.begin(), s.warnings.end(),
                           [](const std::string& w) { return w.find("drum bank") != std::string::npos; });
    };

    // Track 9 stays on the drum channel and selects bank 0 there. The first song's voice gets a copy in the drum bank.
    // The second song's voice can't have one, and gets a warning.
    const MidiEvents a = ReadMidi(first.midi_path);
    const MidiEvents b = ReadMidi(second.midi_path);
    using Slots = std::vector<std::pair<int, int>>;
    SEQLIB_CHECK(first.ok && second.ok);
    SEQLIB_CHECK(a.Find(0x99).size() == 1 && b.Find(0x99).size() == 1);
    SEQLIB_CHECK(Selections(a, 9) == (Slots{{0, 2}}) && Selections(b, 9) == (Slots{{0, 2}}));
    SEQLIB_CHECK(Selections(b, 0) == (Slots{{1, 2}}));
    int drum_presets = 0;
    for (const sf2::Preset& p : shared.File().presets)
    {
        drum_presets += p.bank == 128;
        SEQLIB_CHECK(p.bank != 128 ||
                     (p.program == 2 && PresetInstrument(p) == PresetInstrument(shared.File().presets[0])));
    }

    SEQLIB_CHECK_EQ(drum_presets, 1);
    SEQLIB_CHECK(!warns(first) && warns(second));
}

void TestDrumChannel()
{
    // Two songs whose tracks play one after another. In song 0, track 8 only ends. In song 1, all 16 tracks play.
    Cart cart(kMode, {16});
    const uint32_t voices = cart.VoiceGroup({{2, Cart::Voice(0, cart.Wave(Saw(70)))}});
    for (int song = 0; song < 2; song++)
    {
        std::vector<Track> tracks;
        for (int t = 0; t < 16; t++)
        {
            Track track;
            if (song == 1 || t != 8)
            {
                track.Wait(24 * t).Cmd(kCmdVoice, 2).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24);
            }
            tracks.push_back(track.Fine());
        }
        cart.Song(tracks, voices);
    }

    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("drum_channel");

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    // In song 0, a track plays on every channel after the drum channel. Track 9 moves to channel 9, the last free one
    // before it. Its voice gets no copy in the drum bank. In song 1, track 9 stays on the drum channel. Its voice gets
    // a copy in the drum bank there.
    auto preset_list = [](const std::string& path)
    {
        const Sf2Records sf = ReadSf2(path);
        std::set<std::pair<int, int>> presets;
        for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
        {
            const uint8_t* h = sf.Record("phdr", 38, p);
            presets.insert({Le16(h + 22), Le16(h + 20)});
        }

        return presets;
    };
    using Presets = std::set<std::pair<int, int>>;
    SEQLIB_REQUIRE(first.ok && second.ok);
    const MidiEvents a = ReadMidi(first.midi_path);
    const MidiEvents b = ReadMidi(second.midi_path);
    SEQLIB_CHECK(a.Find(0x99).empty() && a.Find(0x98).size() == 1);
    SEQLIB_CHECK(b.Find(0x99).size() == 1);
    SEQLIB_CHECK(preset_list(first.sf2_path) == (Presets{{0, 2}}));
    SEQLIB_CHECK(preset_list(second.sf2_path) == (Presets{{0, 2}, {128, 2}}));
}

int RunTests()
{
    TestConversion();
    TestSongFiles();
    TestVoiceGroupFiles();
    TestNoteChannels();
    TestPsgNoteTakenBeforeItSounds();
    TestPerformance();
    TestLevels();
    TestMonoLevels();
    TestSlowAttackAtEnd();
    TestTwoVoicesInATick();
    TestSharedSoundfont();
    TestSharedPresets();
    TestSharedDrumChannel();
    TestDrumChannel();

    return seqlib::testing::Summary("mp2k_conversion");
}

} // namespace
} // namespace agblib::sequence_export::mp2k

int main()
{
    return agblib::sequence_export::mp2k::RunTests();
}
