// SPDX-License-Identifier: MIT

// The presets of a Konami song: the keys and zones of each track's sounds, the presets that contain them, and the MIDI
// channels of the tracks. Only convert.cpp includes this header. Its definitions therefore keep internal linkage.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "convert_internal.h"

namespace agblib::sequence_export::konami
{
namespace
{

// Finds the first free key at or above `want`, then searches below it. Returns -1 if all 128 keys are taken.
int FreeKey(const std::set<int>& taken, int want)
{
    for (int key = std::max(want, 0); key <= 127; key++)
    {
        if (!taken.count(key))
        {
            return key;
        }
    }

    for (int key = std::min(want, 128) - 1; key >= 0; key--)
    {
        if (!taken.count(key))
        {
            return key;
        }
    }

    return -1;
}

// Returns true if no zone covers any of the keys lo to hi.
bool RangeFree(const std::vector<KeyZone>& zones, int lo, int hi)
{
    for (const KeyZone& z : zones)
    {
        if (z.lo <= hi && lo <= z.hi)
        {
            return false;
        }
    }

    return true;
}

// Widens a preset's melodic zones over the free keys next to them, to make the preset playable. The keys between two
// zones are split halfway, and the outer zones grow by up to an octave.
void WidenMelodicZones(std::vector<KeyZone>& zones)
{
    std::vector<KeyZone*> mel;
    for (KeyZone& z : zones)
    {
        if (z.melodic)
        {
            mel.push_back(&z);
        }
    }

    std::sort(mel.begin(), mel.end(), [](const KeyZone* a, const KeyZone* b) { return a->lo < b->lo; });

    // Split the keys between two zones halfway, measured from where the lower zone ended before it grew.
    int prev_hi = 0;
    for (size_t i = 0; i < mel.size(); i++)
    {
        KeyZone& z = *mel[i];
        const int floor_key = i == 0 ? std::max(0, z.lo - 12) : (prev_hi + z.lo) / 2 + 1;
        const int ceil_key = i + 1 == mel.size() ? std::min(127, z.hi + 12) : (z.hi + mel[i + 1]->lo) / 2;
        prev_hi = z.hi;

        while (z.lo - 1 >= floor_key && RangeFree(zones, z.lo - 1, z.lo - 1))
        {
            z.lo--;
        }

        while (z.hi + 1 <= ceil_key && RangeFree(zones, z.hi + 1, z.hi + 1))
        {
            z.hi++;
        }
    }
}

// Lays out the samples a DS track plays on the keys of its presets. `usage` gives each sample the semitones it's played
// at.
TrackKeymap BuildKeymap(const std::map<int, std::set<int>>& usage, SoundfontBuilder& sf, const Rom& rom,
                        const DriverInfo& info)
{
    TrackKeymap km;

    // Samples played at several pitches are laid out at their true pitch; ones played at a single pitch (drum style) go
    // wherever there's room.
    std::vector<int> melodic, single;
    for (const auto& [s, semis] : usage)
    {
        (semis.size() > 1 ? melodic : single).push_back(s);
    }

    // The sort is stable so that samples starting on the same key stay in sample order on every platform. std::sort
    // leaves the order of ties to the standard library.
    auto lowest_note_first = [&](int a, int b)
    {
        return sf.DsRootKey(a) + *usage.at(a).begin() < sf.DsRootKey(b) + *usage.at(b).begin();
    };
    std::stable_sort(melodic.begin(), melodic.end(), lowest_note_first);

    // Puts sample s on keys lo to hi of the first preset that has them free, and returns the preset.
    auto place = [&](int s, int lo, int hi, int root, bool is_melodic)
    {
        for (size_t p = 0;; p++)
        {
            if (p == km.presets.size())
            {
                km.presets.emplace_back();
            }

            if (!RangeFree(km.presets[p], lo, hi))
            {
                continue;
            }

            KeyZone z;
            z.sample = sf.DsSample(s);
            z.lo = lo;
            z.hi = hi;
            z.root = root;
            z.loop = info.Sample(rom, s).Looped();
            z.melodic = is_melodic;
            km.presets[p].push_back(z);
            return int(p);
        }
    };

    for (int s : melodic)
    {
        const std::set<int>& semis = usage.at(s);
        int root = sf.DsRootKey(s);
        while (root + *semis.rbegin() > 127 && root >= 12)
        {
            root -= 12;
        }
        while (root + *semis.begin() < 0 && root <= 115)
        {
            root += 12;
        }

        const int lo = std::clamp(root + *semis.begin(), 0, 127);
        const int hi = std::clamp(root + *semis.rbegin(), 0, 127);
        const int p = place(s, lo, hi, root, true);
        for (int k : semis)
        {
            km.notes[{s, k}] = {p, std::clamp(root + k, 0, 127)};
        }
    }

    for (int s : single)
    {
        const int k = *usage.at(s).begin();

        // Prefer the sample's true pitch, then the first free key from C2 up. The zone's root key, k below the key, has
        // to be a MIDI key too. That leaves the keys from k to k + 127. That's none for k = -128: its notes play on key
        // 0 with root key 127, a semitone high.
        const int lowest = std::clamp(k, 0, 127);
        const int highest = std::clamp(127 + k, 0, 127);
        std::vector<int> candidates;
        if (sf.DsPitched(s))
        {
            candidates.push_back(sf.DsRootKey(s) + k);
        }

        for (int key = 36; key < 128; key++)
        {
            candidates.push_back(key);
        }

        for (int key = 35; key >= 0; key--)
        {
            candidates.push_back(key);
        }

        Placement where{-1, 0};
        for (size_t p = 0; p <= km.presets.size() && where.preset < 0; p++)
        {
            if (p == km.presets.size())
            {
                km.presets.emplace_back();
            }

            for (int key : candidates)
            {
                if (key < lowest || key > highest || !RangeFree(km.presets[p], key, key))
                {
                    continue;
                }

                where = {int(p), key};
                break;
            }
        }

        KeyZone z;
        z.sample = sf.DsSample(s);
        z.lo = z.hi = where.key;
        z.root = std::min(where.key - k, 127);
        z.loop = info.Sample(rom, s).Looped();
        km.presets[size_t(where.preset)].push_back(z);
        km.notes[{s, k}] = where;
    }

    for (std::vector<KeyZone>& zones : km.presets)
    {
        WidenMelodicZones(zones);
    }

    return km;
}

// Collects sound usage for the tracks selected by `track_mask`.
Usage CollectUsage(const std::array<std::vector<Event>, kTracks>& events, uint32_t track_mask)
{
    Usage u;
    for (int t = 0; t < kTracks; t++)
    {
        if (!((track_mask >> t) & 1))
        {
            continue;
        }

        for (const Event& e : events[t])
        {
            if (e.type == Event::kNoteOn)
            {
                u.used[size_t(t)] = true;

                switch (e.kind)
                {
                case Kind::kSquare:
                    u.duties.insert(e.param);
                    break;
                case Kind::kWave:
                    u.waves.insert(e.param);
                    break;
                case Kind::kNoise:
                    u.noise_notes.insert(e.param);
                    break;
                case Kind::kSample:
                    u.samples[size_t(t)][e.param].insert(e.note);
                    break;
                }
            }

            if ((e.type == Event::kNoteOn && e.kind != Kind::kNoise) || e.type == Event::kBend)
            {
                u.max_dev[size_t(t)] = std::max(u.max_dev[size_t(t)], std::abs(e.dev));
            }
        }
    }

    return u;
}

// Adds a preset for each instrument the song uses, in the banks and programs that the SoundFont gives song `song`, and
// with `prefix` in front of its name.
Programs AddPresets(const Usage& usage, SoundfontBuilder& sf, const Rom& rom, const DriverInfo& info, int song,
                    const std::string& prefix)
{
    Programs programs;
    Sf2File& file = sf.File();
    sf.Banks().Begin(song);

    auto add_preset = [&](const std::string& name, std::vector<sf2::Zone> zones)
    {
        file.instruments.push_back(MakeInstrument(prefix + name, std::move(zones)));

        const std::optional<BankProgram> slot = sf.Banks().Next();
        programs.fits = programs.fits && slot;
        const sf2::Preset p = MakePreset(prefix + name, uint16_t(slot ? slot->bank : 0),
                                         uint16_t(slot ? slot->program : 0), int(file.instruments.size()) - 1);
        file.presets.push_back(p);

        return BankProgram{p.bank, p.program};
    };

    // The square and wave zones follow the driver's frequency table.
    for (int d : usage.duties)
    {
        const int s = sf.SquareSample(d);
        programs.duty[d] = add_preset(SquareName(d), TunedZones(RootKeyZone(s, true, 0, 127, 60), sf.SquareTuning()));
    }

    for (int row : usage.waves)
    {
        const int w = row >> 4, vol = row & 15;
        const int s = sf.WaveSample(w, vol);
        const int root = std::clamp(sf.WaveKey(w, 36), 0, 127);
        programs.wave[row] =
            add_preset(WaveName(w, vol), TunedZones(RootKeyZone(s, true, 0, 127, root), sf.WaveTuning(w)));
    }

    // Each noise note gets a key of its own: the one 36 above its number, or failing that the nearest free key above
    // it, then below it. A song with more noise notes than there are keys goes on to a second preset.
    std::vector<std::vector<sf2::Zone>> noise_zones;
    std::vector<std::set<int>> noise_taken;
    for (int n : usage.noise_notes)
    {
        Placement where{-1, 0};
        for (size_t p = 0; where.preset < 0; p++)
        {
            if (p == noise_taken.size())
            {
                noise_zones.emplace_back();
                noise_taken.emplace_back();
            }

            const int key = FreeKey(noise_taken[p], 36 + n);
            if (key >= 0)
            {
                where = {int(p), key};
            }
        }

        noise_taken[size_t(where.preset)].insert(where.key);
        programs.noise_keys[n] = where;
        const int sample = sf.NoiseSample(info.NoiseSetting(rom, n));
        noise_zones[size_t(where.preset)].push_back(RootKeyZone(sample, true, where.key, where.key, where.key));
    }

    for (size_t p = 0; p < noise_zones.size(); p++)
    {
        const std::string name = noise_zones.size() > 1 ? "Noise (" + std::to_string(p + 1) + ")" : "Noise";
        programs.noise.push_back(add_preset(name, std::move(noise_zones[p])));
    }

    for (int t = kPsgTracks; t < kTracks; t++)
    {
        if (usage.samples[size_t(t)].empty())
        {
            continue;
        }

        programs.keymaps[size_t(t)] = BuildKeymap(usage.samples[size_t(t)], sf, rom, info);
        const auto& presets = programs.keymaps[size_t(t)].presets;
        for (size_t p = 0; p < presets.size(); p++)
        {
            std::vector<sf2::Zone> zones;
            for (const KeyZone& z : presets[p])
            {
                zones.push_back(RootKeyZone(z.sample, z.loop, z.lo, z.hi, z.root));
            }

            std::string name = TrackName(t);
            if (presets.size() > 1)
            {
                name += " (" + std::to_string(p + 1) + ")";
            }

            programs.tracks[size_t(t)].push_back(add_preset(name, std::move(zones)));
        }
    }

    return programs;
}

// Returns the preset and key that play track t's note-on event `e`.
NoteSound Resolve(const Programs& programs, const SoundfontBuilder& sf, int t, const Event& e)
{
    NoteSound s;
    switch (e.kind)
    {
    case Kind::kSquare:
        s = {programs.duty.at(e.param), sf.SquareBaseKey() + e.note};
        break;

    case Kind::kWave:
        s = {programs.wave.at(e.param), sf.WaveKey(e.param >> 4, e.note)};
        break;

    case Kind::kNoise:
        {
            const Placement& pl = programs.noise_keys.at(e.param);
            s = {programs.noise[size_t(pl.preset)], pl.key};
            break;
        }

    case Kind::kSample:
        {
            const Placement& pl = programs.keymaps[size_t(t)].notes.at({e.param, e.note});
            s = {programs.tracks[size_t(t)][size_t(pl.preset)], pl.key};
            break;
        }
    }

    s.key = std::clamp(s.key, 0, 127);

    return s;
}

// Returns a MIDI channel for each track that plays notes, and -1 for the others. Players often reserve channel 10
// (index 9) for drums. It comes last.
std::array<int, kTracks> AssignChannels(const std::array<bool, kTracks>& used)
{
    constexpr int kOrder[kTracks] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 9};
    std::array<int, kTracks> channel;
    channel.fill(-1);
    int next = 0;
    for (int t = 0; t < kTracks; t++)
    {
        if (used[size_t(t)])
        {
            channel[size_t(t)] = kOrder[next++];
        }
    }

    return channel;
}

} // namespace
} // namespace agblib::sequence_export::konami
