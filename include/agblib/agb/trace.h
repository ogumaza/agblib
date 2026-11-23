// SPDX-License-Identifier: MIT

// The lines of a trace of a driver's model: the records of each frame, each in hex when it has changed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace agblib::agb
{

// Prints `bytes` in hex, two digits a byte.
inline void PrintHex(std::FILE* out, std::span<const uint8_t> bytes)
{
    for (uint8_t b : bytes)
    {
        std::fprintf(out, "%02x", b);
    }
}

// Prints a frame's line for a record that has changed since the last one printed: the frame, `name` and the record in
// hex. Before the first line, the record counts as zeros. `last` keeps the record that was printed last.
inline void PrintChanged(std::FILE* out, long long frame, const char* name, const std::vector<uint8_t>& record,
                         std::vector<uint8_t>& last)
{
    if (last.empty())
    {
        last.assign(record.size(), 0);
    }

    if (record == last)
    {
        return;
    }

    last = record;
    std::fprintf(out, "%lld %s ", frame, name);
    PrintHex(out, record);
    std::fprintf(out, "\n");
}

// A record of a trace's frame: its name and its bytes.
using TraceRecord = std::pair<std::string, std::vector<uint8_t>>;

// Writes a player's state to `out` after each of the first `frames` frames. `frame` runs a frame, or returns false
// without running it once the song has ended. `records` returns the frame's records in the order that they print. Each
// record is written only when it has changed since it was last written.
template <typename Frame, typename Records>
void TraceFrames(std::FILE* out, long long frames, Frame frame, Records records)
{
    std::vector<std::vector<uint8_t>> last;
    for (long long f = 0; f < frames && frame(); f++)
    {
        const std::vector<TraceRecord> now = records();
        if (last.size() < now.size())
        {
            last.resize(now.size());
        }

        for (size_t k = 0; k < now.size(); k++)
        {
            PrintChanged(out, f, now[k].first.c_str(), now[k].second, last[k]);
        }
    }
}

} // namespace agblib::agb
