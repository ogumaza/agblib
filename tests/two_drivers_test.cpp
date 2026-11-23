// SPDX-License-Identifier: MIT

// Tests of a cartridge with two drivers (agblib/testing/two_drivers_cart.h): Nintendo R&D2's driver and MP2K. Each
// driver's detection finds its driver, each cut takes its songs alone, and each join gives the cartridge back. The
// sequence pieces of the two cuts lie apart.

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/mp2k/driver.h"
#include "agblib/mp2k/layout.h"
#include "agblib/mp2k/song.h"
#include "agblib/rd2/driver.h"
#include "agblib/rd2/layout.h"
#include "agblib/testing/mp2k_cart.h"
#include "agblib/testing/rd2_cart.h"
#include "agblib/testing/two_drivers_cart.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/testing/check.h"

using xsflib::testing::Check;

namespace
{

namespace mp2k_cart = agblib::testing::mp2k_cart;
namespace rd2_cart = agblib::testing::rd2_cart;
using agblib::agb::kRomBase;
using agblib::agb::Rom;
using agblib::testing::two_drivers_cart::kMp2kOrigin;
using agblib::testing::two_drivers_cart::TwoDrivers;

// An R&D2 cartridge with two sequences, and an MP2K cartridge past it with one song.
std::vector<uint8_t> TwoDriverBytes()
{
    rd2_cart::Cart rd2;
    rd2.Sequences({{rd2_cart::TrackData().Wait(4).End()}, {rd2_cart::TrackData().End(), rd2_cart::TrackData().End()}});

    mp2k_cart::Cart mp2k(mp2k_cart::kMode, {10, 3}, true, true, kMp2kOrigin);
    const uint32_t voices = mp2k.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, mp2k.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track track;
    track.Cmd(agblib::mp2k::kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine();
    mp2k.Song({track}, voices);

    return TwoDrivers(rd2, mp2k);
}

Rom RomOf(std::vector<uint8_t> bytes)
{
    Rom rom;
    rom.Assign(std::move(bytes));

    return rom;
}

// The places of the sequence pieces of `pieces`, from the ROM's start, each with its end. A sequence piece takes its
// room, and a run its bytes.
std::vector<std::pair<uint64_t, uint64_t>> SequencePlaces(const std::vector<ntdlib::sar::Piece>& pieces)
{
    std::vector<std::pair<uint64_t, uint64_t>> places;
    uint64_t start = 0;
    for (const ntdlib::sar::Piece& piece : pieces)
    {
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&piece))
        {
            start += bytes->size();
        }
        else if (const auto* sequence = std::get_if<ntdlib::sar::SequencePiece>(&piece))
        {
            places.emplace_back(start, start + sequence->room);
            start += sequence->room;
        }
    }

    return places;
}

void TestBothDrivers()
{
    const std::vector<uint8_t> bytes = TwoDriverBytes();
    const Rom rom = RomOf(bytes);

    agblib::mp2k::DriverInfo mp2k_info;
    agblib::rd2::DriverInfo rd2_info;
    std::string error;
    const bool mp2k_found = agblib::mp2k::DetectDriver(rom, {}, mp2k_info, error);
    const bool rd2_found = agblib::rd2::DetectDriver(rom, {}, rd2_info, error);
    Check(mp2k_found, "MP2K's detection finds MP2K");
    Check(rd2_found, "R&D2's detection finds R&D2's driver");
    if (!mp2k_found || !rd2_found)
    {
        return;
    }

    const agblib::mp2k::Layout mp2k = agblib::mp2k::Cut(rom, mp2k_info);
    const agblib::rd2::Layout rd2 = agblib::rd2::Cut(rom, rd2_info);

    Check(mp2k.sequences.size() == 1, "MP2K's cut takes its song");
    Check(rd2.sequences.size() == 2, "R&D2's cut takes its sequences");
    Check(
        agblib::mp2k::Join(mp2k.sequences, mp2k.pieces, mp2k.fields, {.note_arguments = mp2k.note_arguments}) == bytes,
        "MP2K's join gives the cartridge back");
    Check(agblib::rd2::Join(rd2) == bytes, "R&D2's join gives the cartridge back");
    for (const auto& [mp2k_start, mp2k_end] : SequencePlaces(mp2k.pieces))
    {
        Check(mp2k_start >= kMp2kOrigin, "MP2K's song lies past R&D2's cartridge");
        for (const auto& [rd2_start, rd2_end] : SequencePlaces(rd2.pieces))
        {
            Check(mp2k_end <= rd2_start || rd2_end <= mp2k_start, "the sequence pieces of the two cuts lie apart");
        }
    }
}

} // namespace

int main()
{
    TestBothDrivers();

    return xsflib::testing::Summary("two drivers");
}
