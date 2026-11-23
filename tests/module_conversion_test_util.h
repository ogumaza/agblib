// SPDX-License-Identifier: MIT

// What the tests of the module conversions share: a conversion in memory, the reading of its MIDI file, and a count of
// its SoundFont's generators.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "seqlib/midi/builder.h"
#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/sequence_export/agb/settings.h"

namespace agblib::testing::module_conversion_tests
{

// Converts song 0 of `rom` in memory with `convert`, a driver's ConvertSongFiles(), and reads its MIDI file. Sets
// `report` to the conversion's report.
template <typename Info, typename Files>
seqlib::midi::File ConvertToMidi(Files (*convert)(const agblib::agb::Rom&, const Info&, int,
                                                  const agblib::sequence_export::agb::ConvertSettings&),
                                 const agblib::agb::Rom& rom, const Info& info,
                                 agblib::sequence_export::agb::SongReport& report)
{
    agblib::sequence_export::agb::ConvertSettings settings;
    settings.program = "test";
    const Files files = convert(rom, info, 0, settings);
    report = files.report;
    SEQLIB_CHECK(report.ok && !report.silent && !files.midi.empty() && !files.sf2.empty());

    return seqlib::midi::Read(files.midi);
}

// Returns the notes of MIDI channel `channel`.
inline std::vector<seqlib::midi::Note> ChannelNotes(const seqlib::midi::File& midi, int channel)
{
    std::vector<seqlib::midi::Note> notes;
    for (const seqlib::midi::Note& n : seqlib::midi::Notes(midi))
    {
        if (n.channel == channel)
        {
            notes.push_back(n);
        }
    }

    std::sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) { return a.on < b.on; });

    return notes;
}

// Returns true if the file sets the expression of MIDI channel `channel` for a level of `level` on both sides.
inline bool SetsLevel(const seqlib::midi::File& midi, int channel, double level)
{
    int pan = 64, expression = 0;
    seqlib::midi::LevelsToControllers(level, level, pan, expression);
    const std::vector<seqlib::midi::Event> events = midi.Find(uint8_t(0xB0 + channel));

    return std::any_of(events.begin(), events.end(),
                       [&](const seqlib::midi::Event& e) { return e.bytes[1] == 11 && e.bytes[2] == expression; });
}

// Counts the generators of the instruments of the SoundFont `sf` that set `oper` to `amount`.
template <typename Soundfont>
int CountGenerators(const Soundfont& sf, int oper, int amount)
{
    int count = 0;
    for (const auto& instrument : sf.instruments)
    {
        for (const auto& zone : instrument.zones)
        {
            for (const auto& g : zone.generators)
            {
                count += g.oper == oper && g.amount == amount ? 1 : 0;
            }
        }
    }

    return count;
}

} // namespace agblib::testing::module_conversion_tests
