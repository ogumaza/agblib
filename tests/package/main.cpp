// SPDX-License-Identifier: MIT

// Uses agblib from another project: reads the game code from a cartridge header, and splits bytes into data items
// through the headers that include ntdlib's.

#include <cstdint>
#include <map>
#include <vector>

#include "xsflib/testing/check.h"

#include "agblib/agb/data_items.h"
#include "agblib/agb/layout.h"
#include "agblib/agb/rom.h"

int main()
{
    // The cartridge header gives the game code in the 4 bytes at 0xAC.
    std::vector<uint8_t> image(0xc0, 0);
    image[0xac] = 'A';
    image[0xad] = 'G';
    image[0xae] = 'B';
    image[0xaf] = 'E';
    agblib::agb::Rom rom;
    rom.Assign(image);
    XSFLIB_CHECK(rom.GameCode() == "AGBE");

    const std::vector<uint8_t> kData = {1, 2, 3, 4};
    std::vector<ntdlib::commands::Command> items;
    using ItemAt = std::map<uint32_t, uint32_t>;
    const ItemAt at = agblib::agb::AppendDataItems(kData, {2}, items);
    XSFLIB_CHECK(items.size() == 2);
    XSFLIB_CHECK(items[1].bytes == std::vector<uint8_t>({3, 4}));
    XSFLIB_CHECK(at == ItemAt({{0, 0}, {2, 1}}));

    return xsflib::testing::Summary("package");
}
