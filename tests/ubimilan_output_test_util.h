// SPDX-License-Identifier: MIT

// What the tests of ubimilan::Output share: the PSG's registers, the detection of a test image, and a run of its piece
// that keeps each frame's bytes and stores.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/sound_registers.h"
#include "agblib/testing/ubimilan_image.h"
#include "agblib/ubimilan/driver.h"
#include "agblib/ubimilan/output.h"

namespace agblib::testing::ubimilan_output_tests
{

using agblib::agb::RegisterWrite;
using agblib::agb::Rom;
using agblib::testing::ubimilan_image::TestImage;
using agblib::ubimilan::DriverInfo;
using agblib::ubimilan::Output;

using agblib::agb::kRegNr10;
using agblib::agb::kRegNr11;
using agblib::agb::kRegNr13;
using agblib::agb::kRegNr21;
using agblib::agb::kRegNr23;
using agblib::agb::kRegNr30;
using agblib::agb::kRegNr31;
using agblib::agb::kRegNr33;
using agblib::agb::kRegNr41;
using agblib::agb::kRegNr43;

// The bytes of each of Timer 0's frames: 16 bytes in a run, and (frame + 1) * 280,896 / (16 * 1,025) runs by a frame's
// end.
constexpr size_t kRunBytes = 16;

inline DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    XSFLIB_CHECK(agblib::ubimilan::DetectDriver(rom, {}, info, error));
    return info;
}

inline std::vector<int8_t> Constant(size_t points, int point)
{
    return std::vector<int8_t>(points, int8_t(point));
}

// The piece that TestImage's `image` holds as its only piece, played for `frames` frames. Holds the bytes.
struct Played
{
    std::vector<int8_t> bytes;
    std::vector<std::vector<RegisterWrite>> writes;
    bool reset = false;
};

inline Played Play(const TestImage& image, int frames)
{
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info, 0);
    Played played;
    XSFLIB_CHECK(output.Valid());
    for (int frame = 0; frame < frames; frame++)
    {
        output.Frame();
        played.bytes.insert(played.bytes.end(), output.Fifo().begin(), output.Fifo().end());
        played.writes.push_back(output.SoundWrites());
    }

    played.reset = output.Song().Reset();
    return played;
}

// The value of the store to `address` in `writes`, or -1.
inline long long StoreTo(const std::vector<RegisterWrite>& writes, uint32_t address)
{
    long long value = -1;
    for (const RegisterWrite& write : writes)
    {
        if (write.address == address)
        {
            value = write.value;
        }
    }

    return value;
}

// Each store to `address`, in order, with the frame that makes it.
struct FrameStore
{
    bool operator==(const FrameStore&) const = default;

    int frame = 0;
    uint32_t value = 0;
};

inline std::vector<FrameStore> StoresTo(const Played& played, uint32_t address)
{
    std::vector<FrameStore> stores;
    for (size_t frame = 0; frame < played.writes.size(); frame++)
    {
        for (const RegisterWrite& write : played.writes[frame])
        {
            if (write.address == address)
            {
                stores.push_back({int(frame), write.value});
            }
        }
    }

    return stores;
}

} // namespace agblib::testing::ubimilan_output_tests
