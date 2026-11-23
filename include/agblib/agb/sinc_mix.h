// SPDX-License-Identifier: MIT

// The steps that the GBA drivers' enhanced modes share to resample a voice's points for a frame with the windowed sinc
// of ntdlib::wave::SincKernel. A voice's position counts points in 32.32 fixed point.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/wave/sinc.h"

namespace agblib::agb
{

// The kernel for a frame of `samples` outputs as the position moves by `move`.
inline const ntdlib::wave::SincKernel& FrameKernel(uint64_t move, size_t samples)
{
    return ntdlib::wave::SincKernel::ForStep(double(move) / (double(samples) * 4294967296.0));
}

// The points that a frame's outputs read with `kernel`: the voice's points from the kernel's first input for the
// frame's start to its last input for the frame's end.
class SincPoints
{
public:
    // Reads the points for the outputs from `start` as the position moves by `move`. `point_of(n)` gives the voice's
    // point n. That point may be before the voice's first point or after its last.
    template <typename PointOf>
    SincPoints(const ntdlib::wave::SincKernel& kernel, uint64_t start, uint64_t move, const PointOf& point_of)
        : kernel_(kernel), width_(kernel.HalfWidth()), first_(int64_t(start >> 32) - (width_ - 1))
    {
        const int64_t last = int64_t((start + move) >> 32) + width_;
        input_.resize(size_t(last - first_ + 1));
        for (int64_t n = first_; n <= last; n++)
        {
            input_[size_t(n - first_)] = point_of(n);
        }
    }

    // The output at `position`. The position is between the frame's start and its end.
    float At(uint64_t position) const
    {
        const int64_t point = int64_t(position >> 32);
        return kernel_.At(&input_[size_t(point - (width_ - 1) - first_)], uint32_t(position >> 16) & 0xFFFF);
    }

private:
    const ntdlib::wave::SincKernel& kernel_;
    int64_t width_;
    int64_t first_;
    std::vector<float> input_;
};

// Adds a voice's `samples` outputs from `start`, as the position moves by `move`, to the sides `a` and `b`. The voice's
// levels on the two sides move in a straight line from `from` to `to` over the frame. `point_of(n)` gives the voice's
// point n. `meter(part_a, part_b)` takes each output's parts of the two sides.
template <typename PointOf, typename Meter>
void MixSincRamp(uint64_t start, uint64_t move, size_t samples, std::array<float, 2> from, std::array<float, 2> to,
                 float* a, float* b, const PointOf& point_of, const Meter& meter)
{
    const SincPoints points(FrameKernel(move, samples), start, move, point_of);
    const std::array<float, 2> slopes = {(to[0] - from[0]) / float(samples), (to[1] - from[1]) / float(samples)};
    for (size_t i = 0; i < samples; i++)
    {
        const float value = points.At(start + uint64_t(i) * move / samples);
        const float part_a = value * (from[0] + float(i) * slopes[0]);
        const float part_b = value * (from[1] + float(i) * slopes[1]);
        a[i] += part_a;
        b[i] += part_b;
        meter(part_a, part_b);
    }
}

} // namespace agblib::agb
