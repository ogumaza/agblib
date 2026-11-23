// SPDX-License-Identifier: MIT

// Unit tests for the mixer of MP2K in Camelot's games: its channels, its synth voices, its output stage and its echo,
// in the earlier and the later build. They build small synthetic cartridges in memory. No game ROM is needed.

#include "seqlib/testing/check.h"

#include "mp2k_test_util.h"

namespace agblib::sequence_export::mp2k
{
namespace
{

using seqlib::testing::Throws;

// The points that the mixer of Golden Sun: The Lost Age mixes in a frame, at the rate of kCamelotMode.
constexpr int kCamelotPoints = 528;

// Camelot's mixer takes 256 less the release off the level each frame. It leaves out a note that comes out silent on
// both sides. Such a note doesn't move on through its sample. The mixer plays a sample of no length as a synth voice,
// which goes on until it's released: a pulse wave moves its duty on each frame, and a saw wave keeps its filter in the
// count.
void TestCamelotMixer()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t pulse = cart.Synth(0, {0x80, 0x10, 0x00, 0x00});
    const uint32_t saw = cart.Synth(1, {});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 200})},
                                             {1, Cart::Voice(0, pulse)},
                                             {2, Cart::Voice(0, saw)}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(4, 60, 127).Wait(12).Fine()},
              voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 1).Tie(60, 1).Wait(12).Fine()}, voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Tie(60, 127).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 2).Cmd(kCmdVol, 127).Tie(60, 127).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // The note's level after each frame from its release, on frame 4, until it stops.
    Sequencer release(rom, info, 0);
    std::vector<int> levels;
    for (int f = 0; f < 9; f++)
    {
        release.Step();
        if (f >= 4)
        {
            levels.push_back(release.GetChannel(2).status ? release.GetChannel(2).envelope : -1);
        }
    }

    Sequencer silent(rom, info, 1);
    for (int f = 0; f < 3; f++)
    {
        silent.Step();
    }

    // The model refuses an echo that doesn't reach back within a frame.
    DriverInfo far_echo = info;
    far_echo.camelot_tap = info.samples_per_frame;
    const bool refused = Throws<std::invalid_argument>([&] { Sequencer(rom, far_echo, 0).StartMixing(); });

    Sequencer synths(rom, info, 2);
    synths.Step();
    const int32_t saw_filter = synths.GetChannel(3).count;
    synths.Step();
    synths.Step();
    const std::vector<int16_t> saw_points = RenderSynth(rom, info, Synth::kSaw, saw + 16, 13379, 224);

    const std::vector<int> kLevels = {199, 143, 87, 31, -1};
    SEQLIB_CHECK(levels == kLevels);
    SEQLIB_CHECK(refused);
    SEQLIB_CHECK(silent.GetChannel(2).status && silent.GetChannel(2).count == 64);

    // The pulse wave's phase moves on by 8 times a sample's step for each of the mixer's 224 points a frame, and its
    // duty's phase by its fourth byte. The saw wave's filter ends the frame where the SoundFont's sample does.
    const Channel& p = synths.GetChannel(2);
    SEQLIB_CHECK(p.status && p.synth && uint32_t(p.count) == 0x30000000u);
    SEQLIB_CHECK_EQ(p.fraction, uint32_t(3 * uint64_t((13379u * 627u) << 3) * 224 % 0x100000000u));
    SEQLIB_CHECK(synths.GetChannel(3).status && saw_points.size() == 224 && saw_points[223] == saw_filter * 128);
}

// Returns the volumes at which Camelot's mixer plays channel `c`, right then left: each side's volume times 9/8 of
// the envelope, over 512 and rounded down (0x03000846 in Golden Sun: The Lost Age's mixer).
std::array<int32_t, 2> CamelotVolumes(const Channel& c)
{
    const int32_t level = c.envelope + (c.envelope >> 3);
    return {(c.right * level) >> 9, (c.left * level) >> 9};
}

// Returns the two points that Camelot's output stage writes for a side's sum, after the 64 that rounds it when `round`
// (0x03000d40). A negative sum takes 64 more. The first point is the sum over 128, rounded down, and the second that of
// the sum less 64.
std::array<int8_t, 2> CamelotPoints(int32_t sum, bool round)
{
    sum += round ? 64 : 0;
    sum += sum < 0 ? 64 : 0;
    return {int8_t(sum >> 7), int8_t((sum - 64) >> 7)};
}

// Works out the points that Camelot's output stage writes over whole frames, side by side, from what the channels add
// to each sum: `added[f][side][i]`, the right side then the left. `many[f]` says whether frame f mixed 4 channels or
// more: only the first two sums then take the 64 that rounds them. Each frame's sums start with an echo of the points
// before (0x03000e00 to 0x03000e60). Each side takes 53/128 of its first points from 4 frames before, less 8/128 of
// the other side's. The right side also takes a quarter of its second points from the frame before, plus 1/128 of that.
// The left side takes a quarter of the right side's first points from 528 + 176 points before. The right side's sums
// have to stay from 64 to 16319, and the left side's from 0: the output stage then neither saturates nor carries from
// one side to the other.
MixedFrames CamelotFrames(const std::vector<std::array<std::vector<int32_t>, 2>>& added, const std::vector<bool>& many)
{
    constexpr size_t kPoints = size_t(kCamelotPoints);
    constexpr size_t kLeftEcho = 176;
    const size_t frames = added.size();
    std::array<std::vector<int32_t>, 2> first = {std::vector<int32_t>(frames * kPoints),
                                                 std::vector<int32_t>(frames * kPoints)};
    std::vector<int32_t> second(frames * kPoints);
    MixedFrames out;
    for (size_t f = 0; f < frames; f++)
    {
        out.right.emplace_back(2 * kPoints);
        out.left.emplace_back(2 * kPoints);
        for (size_t i = 0; i < kPoints; i++)
        {
            const size_t n = f * kPoints + i;
            std::array<int32_t, 2> sums = {added[f][0][i], added[f][1][i]};
            if (f >= 4)
            {
                const size_t back = n - 4 * kPoints;
                sums[0] += 53 * first[0][back] - 8 * first[1][back];
                sums[1] += 53 * first[1][back] - 8 * first[0][back];
            }

            if (f >= 1)
            {
                sums[0] += 32 * second[n - kPoints] + second[n - kPoints] / 4;
            }

            if (n >= kPoints + kLeftEcho)
            {
                sums[1] += 32 * first[0][n - kPoints - kLeftEcho];
            }

            SEQLIB_CHECK(sums[0] >= 64 && sums[0] < 16320 && sums[1] >= 0 && sums[1] < 16320);

            const bool round = i < 2 || !many[f];
            const std::array<int8_t, 2> right = CamelotPoints(sums[0], round);
            const std::array<int8_t, 2> left = CamelotPoints(sums[1], round);
            first[0][n] = right[0];
            first[1][n] = left[0];
            second[n] = right[1];
            out.right[f][2 * i] = right[0];
            out.right[f][2 * i + 1] = right[1];
            out.left[f][2 * i] = left[0];
            out.left[f][2 * i + 1] = left[1];
        }
    }

    return out;
}

// Returns the values that Camelot's mixer takes from a sample whose points are all `point`, in a frame from the
// sample's start, at `step` 2^-23 points for each of the frame's points (0x03000940). The sample has to contain more
// points than the frame takes. Below a step of 1, the values are twice the points. The step goes on to the next point
// when the fraction, kept less 1, reaches 0, and at each point from a step of 1 up. The first value of each 8 is the
// point. A later one that comes just after the step goes on to a point is the last value shifted right by 22 or 23 bits
// instead, added to the point: 0, or -1 when that value was negative.
std::vector<int32_t> CamelotConstantValues(int32_t point, uint32_t step)
{
    const bool below_one = step < 0x800000;
    std::vector<int32_t> values;
    uint32_t fraction = 0;
    bool moved = false;
    for (int i = 0; i < kCamelotPoints; i++)
    {
        const int32_t value = below_one ? 2 * point : point;
        const bool shifted = i % 8 != 0 && moved && !values.empty() && values.back() < 0;
        values.push_back(shifted ? value - 1 : value);

        moved = true;
        if (below_one)
        {
            fraction += step - 0x800000;
            moved = int32_t(fraction) >= 0;
            fraction &= 0x7FFFFF;
        }
    }

    return values;
}

// Returns the value that Camelot's mixer takes from point `k` of `pcm` and the next point, at `fraction` of the way
// from one to the other in 2^-23 points. The value is twice the point interpolated to 9 bits when `nine_bits`, and
// otherwise the point interpolated to 8 bits (0x03000940 and 0x03000b44).
int32_t CamelotValue(const std::vector<int8_t>& pcm, size_t k, uint32_t fraction, bool nine_bits)
{
    const int32_t point = pcm[k];
    const int32_t product = int32_t(fraction * uint32_t(pcm[k + 1] - point));
    return nine_bits ? 2 * point + (product >> 22) : point + (product >> 23);
}

// The channels of a sequencer that sound, in order.
std::vector<Channel> Sounding(const Sequencer& seq, const DriverInfo& info)
{
    std::vector<Channel> channels;
    for (int i = 0; i < info.max_channels; i++)
    {
        if (seq.GetChannel(i).status)
        {
            channels.push_back(seq.GetChannel(i));
        }
    }

    return channels;
}

// Camelot's mixer adds each channel's points to sums of 16 bits a side, and its output stage writes two points for
// each sum. Each frame's sums start with the echo of the points before. A fixed-pitch sample plays at the channel's
// volumes, point for point, through its loop. A sum past 15 bits saturates. Other samples play at half the volumes,
// rounded up, interpolated between their points. A sample that ends in a frame stops its channel there. A synth voice
// plays a pulse or triangle wave at the full volumes, and a saw wave at half of them, rounded down.
void TestCamelotMixing()
{
    Cart cart(kCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    std::vector<int8_t> ramp(300);
    for (size_t k = 0; k < ramp.size(); k++)
    {
        ramp[k] = int8_t(4 + k % 37);
    }

    const uint32_t looped = cart.Wave(ramp, 31536, 100);
    const uint32_t three = cart.Wave(std::vector<int8_t>(64, 3), 31536, 0);
    const uint32_t high = cart.Wave(std::vector<int8_t>(64, 127), 31536, 0);
    const uint32_t low = cart.Wave(std::vector<int8_t>(64, -128), 31536, 0);
    const std::vector<int8_t> negative(4000, -10);
    const std::array<uint32_t, 3> rates = {31536, 47304, 70000};
    const std::array<uint32_t, 3> interpolated = {cart.Wave(negative, rates[0], 0), cart.Wave(negative, rates[1], 0),
                                                  cart.Wave(negative, rates[2], 0)};
    // A sample of 200 points of 20 without a loop. The byte after its end is 20 too.
    const uint32_t ending = cart.Wave(std::vector<int8_t>(201, 20), 31536);
    cart.Put32(ending + 12, 200);
    const std::vector<int8_t> jumpy = Jumpy(2001);
    const std::array<uint32_t, 3> jumping = {cart.Wave(jumpy, rates[0], 0), cart.Wave(jumpy, rates[1], 0),
                                             cart.Wave(jumpy, rates[2], 0)};
    // A sample of 50 points that loops from point 10. The tools write the loop's first point after a looped sample's
    // end.
    std::vector<int8_t> short_loop = Jumpy(50);
    short_loop.push_back(short_loop[10]);
    const uint32_t short_looped = cart.Wave(short_loop, rates[2], 10);
    cart.Put32(short_looped + 12, 50);
    const uint32_t pulse = cart.Synth(0, {0x80, 0x10, 0x40, 0x90});
    const uint32_t saw = cart.Synth(1, {});
    const uint32_t triangle = cart.Synth(2, {});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, looped)},
                                             {1, Cart::Voice(kFixed, three)},
                                             {2, Cart::Voice(kFixed, high)},
                                             {3, Cart::Voice(kFixed, low)},
                                             {4, Cart::Voice(0, interpolated[0])},
                                             {5, Cart::Voice(0, interpolated[1])},
                                             {6, Cart::Voice(0, interpolated[2])},
                                             {7, Cart::Voice(0, ending)},
                                             {8, Cart::Voice(0, pulse)},
                                             {9, Cart::Voice(0, saw)},
                                             {10, Cart::Voice(0, triangle)},
                                             {11, Cart::Voice(0, jumping[0])},
                                             {12, Cart::Voice(0, jumping[1])},
                                             {13, Cart::Voice(0, jumping[2])},
                                             {14, Cart::Voice(0, short_looped)}});
    const auto held = [](int voice, int pan = 0x40, int velocity = 127)
    {
        return Track().Cmd(kCmdVoice, voice).Cmd(kCmdVol, 127).Cmd(kCmdPan, pan).Tie(60, velocity).Wait(48).Fine();
    };
    cart.Song({held(0, 0x50)}, voices);
    cart.Song({held(1), held(1), held(1), held(1)}, voices);
    cart.Song({held(2), held(2)}, voices);
    cart.Song({held(3), held(3)}, voices);
    // At velocity 122, both sides' volumes are odd, and their halves round up.
    for (int voice = 4; voice <= 14; voice++)
    {
        cart.Song({held(voice, 0x40, 122)}, voices);
    }

    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_CHECK(info.camelot_mixer && info.samples_per_frame == kCamelotPoints);

    // Mixes each of the first frames of a song, and works out what its channels add to the sums in each, from a
    // channel's volumes and its sample's point for each of the frame's points.
    using Added = std::array<std::vector<int32_t>, 2>;
    const auto mix_fixed = [&](int song, int frames, const auto& point)
    {
        Sequencer seq(rom, info, song);
        seq.StartMixing();
        MixedFrames mixed;
        std::vector<Added> added;
        std::vector<bool> many;
        for (int f = 0; f < frames; f++)
        {
            seq.Step();
            mixed.right.emplace_back(seq.Right().begin(), seq.Right().end());
            mixed.left.emplace_back(seq.Left().begin(), seq.Left().end());
            const std::vector<Channel> sounding = Sounding(seq, info);
            Added frame = {std::vector<int32_t>(kCamelotPoints), std::vector<int32_t>(kCamelotPoints)};
            for (const Channel& c : sounding)
            {
                const std::array<int32_t, 2> volumes = CamelotVolumes(c);
                for (size_t i = 0; i < size_t(kCamelotPoints); i++)
                {
                    const int32_t value = point(size_t(f) * size_t(kCamelotPoints) + i);
                    frame[0][i] += volumes[0] * value;
                    frame[1][i] += volumes[1] * value;
                }
            }

            added.push_back(frame);
            many.push_back(sounding.size() >= 4);
        }

        return std::pair{mixed, CamelotFrames(added, many)};
    };

    // The looped sample plays its 300 points, then the 200 from its loop's start again and again. The echo of the
    // first frames comes back in later ones. Four channels sum the second song's points, and only the first two sums
    // of each frame take the 64 that rounds them.
    const auto [looping, looping_frames] =
        mix_fixed(0, 12, [&](size_t n) { return ramp[n < 300 ? n : 100 + (n - 300) % 200]; });
    const auto [four, four_frames] = mix_fixed(1, 6, [](size_t) { return 3; });
    SEQLIB_CHECK(looping.right == looping_frames.right && looping.left == looping_frames.left);
    SEQLIB_CHECK(four.right == four_frames.right && four.left == four_frames.left);
    SEQLIB_CHECK(looping.right[0] != looping.left[0]);

    // Two channels at the top or the bottom of the range go past 15 bits on each side.
    for (int song = 2; song <= 3; song++)
    {
        Sequencer seq(rom, info, song);
        const MixedFrames m = Mix(seq, 1);
        const std::vector<Channel> sounding = Sounding(seq, info);
        const int8_t end = song == 2 ? 127 : -128;
        const int32_t sum = 2 * CamelotVolumes(sounding[0])[0] * 127;
        SEQLIB_CHECK(sounding.size() == 2 && sum > 16383 && sum < 32768);
        SEQLIB_CHECK(std::all_of(m.right[0].begin(), m.right[0].end(), [&](int8_t p) { return p == end; }) &&
                     m.left[0] == m.right[0]);
    }

    // The other samples play at half the volumes, rounded up, at the three steps' codes. Each point of a long sample
    // comes from CamelotConstantValues(). A step from 1 up interpolates to 8 bits at twice the halved volume. The left
    // side borrows from a negative right side.
    std::array<int32_t, 2> volumes = {};
    for (size_t r = 0; r < rates.size(); r++)
    {
        Sequencer seq(rom, info, 4 + int(r));
        const MixedFrames m = Mix(seq, 1);
        const Channel& c = seq.GetChannel(2);
        volumes = CamelotVolumes(c);
        const uint32_t step = c.frequency * info.step_scale;
        const uint32_t advance = uint32_t((uint64_t(step) * uint64_t(kCamelotPoints)) >> 23);
        const int32_t times = r == 0 ? 1 : 2;
        const std::array<int32_t, 2> scales = {times * ((volumes[0] + 1) / 2), times * ((volumes[1] + 1) / 2)};
        const std::vector<int32_t> values = CamelotConstantValues(-10, step);
        bool same = true;
        for (size_t i = 0; i < values.size(); i++)
        {
            const std::array<int8_t, 2> right = CamelotPoints(scales[0] * values[i], true);
            const std::array<int8_t, 2> left = CamelotPoints(scales[1] * values[i] - 1, true);
            same = same && m.right[0][2 * i] == right[0] && m.right[0][2 * i + 1] == right[1] &&
                   m.left[0][2 * i] == left[0] && m.left[0][2 * i + 1] == left[1];
        }

        SEQLIB_CHECK(same);
        SEQLIB_CHECK(c.frequency == rates[r] && (step < 0x800000) == (r == 0) && (step < 0x1000000) == (r < 2));
        SEQLIB_CHECK(c.count == int32_t(4000 - advance) && c.position == interpolated[r] + 16 + advance);
        SEQLIB_CHECK(volumes[0] % 2 == 1 && volumes[1] % 2 == 1 &&
                     std::count(values.begin(), values.end(), r == 0 ? -21 : -11) > 0);
    }

    // A sample whose neighbouring points all differ plays the value between the point at its position and the next
    // point, by the position's fraction. Below a step of 1, the value is interpolated to 9 bits, and from 1 up to 8
    // bits at twice the volume. A looped sample with fewer points left than the frame takes plays point by point to 9
    // bits. At its end, it goes back to its loop's start, past it by the points that the step overshot.
    for (size_t r = 0; r <= rates.size(); r++)
    {
        Sequencer seq(rom, info, 11 + int(r));
        const MixedFrames m = Mix(seq, 1);
        const Channel& c = seq.GetChannel(2);
        const std::array<int32_t, 2> v = CamelotVolumes(c);
        const uint32_t step = c.frequency * info.step_scale;
        const bool nine_bits = r == 0 || r == rates.size();
        const int32_t times = nine_bits ? 1 : 2;
        const std::vector<int8_t>& pcm = r < rates.size() ? jumpy : short_loop;
        bool same = true;
        for (size_t i = 0; i < size_t(kCamelotPoints); i++)
        {
            const uint64_t position = uint64_t(i) * step;
            size_t k = size_t(position >> 23);
            if (r == rates.size() && k >= 50)
            {
                k = 10 + (k - 50) % 40;
            }

            const int32_t value = CamelotValue(pcm, k, uint32_t(position & 0x7FFFFF), nine_bits);
            const std::array<int32_t, 2> sums = {times * ((v[0] + 1) / 2) * value, times * ((v[1] + 1) / 2) * value};
            const std::array<int8_t, 2> right = CamelotPoints(sums[0], true);
            const std::array<int8_t, 2> left = CamelotPoints(sums[1] - (sums[0] < 0 ? 1 : 0), true);
            same = same && m.right[0][2 * i] == right[0] && m.right[0][2 * i + 1] == right[1] &&
                   m.left[0][2 * i] == left[0] && m.left[0][2 * i + 1] == left[1];
        }

        SEQLIB_CHECK(same && c.status);
    }

    // A sample with fewer points left than the frame takes plays point by point, and stops its channel at its end: its
    // 200 points, and the point where the fraction first reaches 1.
    {
        Sequencer seq(rom, info, 7);
        const MixedFrames m = Mix(seq, 1);
        bool same = true;
        for (size_t i = 0; i < size_t(kCamelotPoints); i++)
        {
            const std::array<int8_t, 2> right = CamelotPoints(i <= 200 ? (volumes[0] + 1) / 2 * 40 : 0, true);
            const std::array<int8_t, 2> left = CamelotPoints(i <= 200 ? (volumes[1] + 1) / 2 * 40 : 0, true);
            same = same && m.right[0][2 * i] == right[0] && m.right[0][2 * i + 1] == right[1] &&
                   m.left[0][2 * i] == left[0] && m.left[0][2 * i + 1] == left[1];
        }

        SEQLIB_CHECK(same && !seq.GetChannel(2).status);
    }

    // The synth voices: the pulse wave as the SoundFont's sample plays it, the triangle wave rising by a 256th of its
    // range for each 2^23 of the phase and falling after half the cycle, and the saw wave through its filter.
    for (int synth = 0; synth < 3; synth++)
    {
        Sequencer seq(rom, info, 8 + synth);
        const MixedFrames m = Mix(seq, 1);
        const Channel& c = seq.GetChannel(2);
        const std::array<int32_t, 2> v = CamelotVolumes(c);
        const uint32_t step = SynthStep(c.frequency, info.step_scale);
        const std::vector<int16_t> rendered =
            synth == 2 ? std::vector<int16_t>{}
                       : RenderSynth(rom, info, synth == 0 ? Synth::kPulse : Synth::kSaw,
                                     (synth == 0 ? pulse : saw) + 16, c.frequency, uint32_t(kCamelotPoints));
        bool same = true;
        int32_t lowest = 0;
        for (size_t i = 0; i < size_t(kCamelotPoints); i++)
        {
            std::array<int32_t, 2> sums = {};
            if (synth == 0)
            {
                sums = {v[0] * (rendered[i] / 256), v[1] * (rendered[i] / 256)};
            }
            else if (synth == 1)
            {
                sums = {(v[0] >> 1) * (rendered[i] / 128), (v[1] >> 1) * (rendered[i] / 128)};
            }
            else
            {
                const uint32_t phase = uint32_t(i + 1) * step;
                const int32_t value = int32_t(phase) >= 0 ? int32_t(phase >> 23) - 128 : 384 - int32_t(phase >> 23);
                sums = {v[0] * value, v[1] * value};
            }

            lowest = std::min(lowest, sums[0]);
            const std::array<int8_t, 2> right = CamelotPoints(sums[0], true);
            const std::array<int8_t, 2> left = CamelotPoints(sums[1] - (sums[0] < 0 ? 1 : 0), true);
            same = same && m.right[0][2 * i] == right[0] && m.right[0][2 * i + 1] == right[1] &&
                   m.left[0][2 * i] == left[0] && m.left[0][2 * i + 1] == left[1];
        }

        SEQLIB_CHECK(same && c.synth && lowest < 0);
    }

    // The player's timer takes two points from the FIFOs for each of the mixer's: every 280,896 / 1,056 = 266 cycles.
    // As in TestPlayerFifo(), the first frame plays the silent part of the buffer and the FIFOs' lead, and each output
    // sample, 256 cycles after the last, plays the point of the last overflow at or before it.
    const uint32_t period = uint32_t(agblib::agb::kFrameCycles) / uint32_t(2 * kCamelotPoints);
    std::vector<int8_t> right(size_t(2 * kCamelotPoints + kFifoLead), 0);
    std::vector<int8_t> left = right;
    ArchivePlayer player(rom, info);
    player.Start(0);
    std::vector<int16_t> out;
    for (size_t f = 0; f < looping.right.size(); f++)
    {
        right.insert(right.end(), looping.right[f].begin(), looping.right[f].end());
        left.insert(left.end(), looping.left[f].begin(), looping.left[f].end());
        player.RunFrame(out);
    }

    bool matches = true;
    for (size_t k = 0; k < out.size() / 2; k++)
    {
        const size_t pop = k * 256 / period;
        matches = matches && out[2 * k] == 256 * left[pop] && out[2 * k + 1] == 256 * right[pop];
    }

    SEQLIB_CHECK(period == 266 && matches && out.size() == 2 * looping.right.size() * agblib::agb::kFrameCycles / 256);
}

// A later version of Camelot's driver mixes the mode's points a frame, 12 channels whatever the mode says, and writes
// one point for each of the mixer's. The echo's levels and the left side's tap come from the output stage's literals
// (0x03000dd8 and 0x03000de0 in Mario Tennis: Power Tour).
void TestLaterCamelotMixing()
{
    constexpr int kPoints = 264;
    constexpr int kTap = 100;
    Cart cart(kLaterCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    cart.WriteLaterCamelot(kTap, 32, -6);
    const uint32_t constant = cart.Wave(std::vector<int8_t>(40 * kPoints, 50), 15768);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, constant)}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Tie(60, 127).Wait(96).Eot(60).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_CHECK_EQ(info.samples_per_frame, kPoints);
    SEQLIB_CHECK_EQ(info.max_channels, 12);
    SEQLIB_CHECK_EQ(info.camelot_output_points, 1);
    SEQLIB_CHECK_EQ(info.camelot_echo, 32);
    SEQLIB_CHECK_EQ(info.camelot_cross_echo, -6);
    SEQLIB_CHECK_EQ(info.camelot_tap, kTap);
    SEQLIB_CHECK(info.camelot_adpcm && info.camelot_top_priority);

    // Each frame's sums start with 32/128 of each side's points from 7 frames before, less 6/128 of the other side's.
    // The right side takes a quarter of the last frame's right side, and the left side a quarter of the right side from
    // kTap points before. Each start takes 64 less, and the output stage's rounding gives it back: only the first
    // frame's sums end up 64 higher. The sums stay positive and fit in 15 bits. No side then carries into the other. A
    // point is bits 7 to 14 of the sum.
    Sequencer seq(rom, info, 0);
    seq.StartMixing();
    std::array<std::vector<std::vector<int32_t>>, 2> output;
    const auto point = [&](size_t side, int frame, int i)
    {
        return frame >= 0 ? output[side][size_t(frame)][size_t(i)] : 0;
    };
    bool matches = true;
    bool in_range = true;
    for (int f = 0; f < 30; f++)
    {
        seq.Step();
        const std::vector<Channel> sounding = Sounding(seq, info);
        SEQLIB_REQUIRE(sounding.size() == 1);
        const std::array<int32_t, 2> volumes = CamelotVolumes(sounding[0]);
        SEQLIB_REQUIRE(int(seq.Right().size()) == kPoints && int(seq.Left().size()) == kPoints);
        output[0].emplace_back(kPoints);
        output[1].emplace_back(kPoints);
        for (int i = 0; i < kPoints; i++)
        {
            const int32_t tap = i >= kTap ? point(0, f - 1, i - kTap) : point(0, f - 2, kPoints - kTap + i);
            const int32_t right = 32 * point(0, f - 7, i) - 6 * point(1, f - 7, i) + 32 * point(0, f - 1, i);
            const int32_t left = 32 * point(1, f - 7, i) - 6 * point(0, f - 7, i) + 32 * tap;
            const int32_t round = f == 0 ? 64 : 0;
            const std::array<int32_t, 2> sums = {right + 50 * volumes[0] + round, left + 50 * volumes[1] + round};
            in_range = in_range && sums[0] >= 64 && sums[0] < 16320 && sums[1] >= 64 && sums[1] < 16320;
            output[0][size_t(f)][size_t(i)] = sums[0] >> 7;
            output[1][size_t(f)][size_t(i)] = sums[1] >> 7;
            matches = matches && seq.Right()[size_t(i)] == sums[0] >> 7 && seq.Left()[size_t(i)] == sums[1] >> 7;
        }
    }

    SEQLIB_CHECK(in_range);
    SEQLIB_CHECK(matches);

    // A MIDI file's reverb takes the echo's level.
    ConvertSettings opt = TempSettings("later_camelot");
    const SongReport report = ConvertSong(rom, info, 0, opt, nullptr);
    SEQLIB_CHECK(report.ok);
    const MidiEvents midi = ReadMidi(report.midi_path);
    int reverbs = 0;
    for (const auto& e : midi.Find(uint8_t(0xB0)))
    {
        if (e.bytes[1] == cc::kReverb)
        {
            reverbs++;
            SEQLIB_CHECK_EQ(e.bytes[2], 32);
        }
    }

    SEQLIB_CHECK_EQ(reverbs, 1);
}

// A later version of Camelot's mixer plays a fixed-pitch sample of a negative size in 4-bit steps, a byte for each two
// points, the high nibble first (0x03000c1c in Mario Tennis: Power Tour). The index starts at 56, and a step's value
// shifts right by half the index. The index then moves by 4 less the step's size. The channel's fraction keeps the
// index and the point between frames, and its type marks it. Such a sample has no loop.
void TestLaterCamelotSteps()
{
    constexpr int kPoints = 264;
    Cart cart(kLaterCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    cart.WriteLaterCamelot(88, 32, -6);

    // +4 and -4 keep the index at 56 and a value of 1 at a shift of 28: the points go 4, 8, 4, 8.
    std::vector<uint8_t> even = {0x44};
    even.resize(kPoints, 0xC4);

    // By hand: +4 to 4, +4 to 8, +4 to 12, -8 to 4 with an index of 52. At that index, a shift of 26 makes the next
    // step of 1 worth 4, to 8, with an index of 55. A step of 0 then changes nothing.
    const std::vector<uint8_t> by_hand = {0x44, 0x48, 0x10};

    // After a point of 8, steps of 0 keep it there.
    std::vector<uint8_t> level = {0x44};
    level.resize(10 * kPoints, 0x00);

    for (const auto& [steps, points] :
         {std::pair{even, 2 * kPoints}, std::pair{by_hand, 6}, std::pair{level, 20 * kPoints}})
    {
        const uint32_t wave = cart.StepWave(steps, uint32_t(points));
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, wave)}});
        cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Tie(60, 127).Wait(96).Eot(60).Fine()},
                  voices);
    }

    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer even_song(rom, info, 0);
    even_song.StartMixing();
    even_song.Step();
    const std::vector<Channel> sounding = Sounding(even_song, info);
    SEQLIB_REQUIRE(sounding.size() == 1);
    const std::array<int32_t, 2> volumes = CamelotVolumes(sounding[0]);
    bool even_points = true;
    for (int i = 0; i < kPoints; i++)
    {
        const int32_t value = i % 2 == 0 ? 4 : 8;
        even_points = even_points && even_song.Right()[size_t(i)] == (volumes[0] * value + 64) >> 7 &&
                      even_song.Left()[size_t(i)] == (volumes[1] * value + 64) >> 7;
    }

    SEQLIB_CHECK(even_points);
    SEQLIB_CHECK_EQ(sounding[0].count, kPoints);
    SEQLIB_CHECK_EQ(sounding[0].fraction, 56u | 8u << 16);
    SEQLIB_CHECK(sounding[0].type & kVoiceCompressed);
    even_song.Step();
    SEQLIB_CHECK(Sounding(even_song, info).empty());

    Sequencer hand_song(rom, info, 1);
    hand_song.StartMixing();
    hand_song.Step();
    const std::array<int32_t, 6> kPointsByHand = {4, 8, 12, 4, 8, 8};
    bool hand_points = true;
    for (size_t i = 0; i < kPointsByHand.size(); i++)
    {
        hand_points = hand_points && hand_song.Right()[i] == (volumes[0] * kPointsByHand[i] + 64) >> 7;
    }

    SEQLIB_CHECK(hand_points);
    SEQLIB_CHECK_EQ(hand_song.Right()[6], 0);
    SEQLIB_CHECK(Sounding(hand_song, info).empty());

    // The echo dies away, and the mixer falls silent.
    SEQLIB_CHECK(!hand_song.MixerSilent());
    for (int f = 0; f < 100; f++)
    {
        hand_song.Step();
    }

    SEQLIB_CHECK(hand_song.MixerSilent());

    // A SoundFont contains the decoded points, at the mixer's rate.
    ConvertSettings opt = TempSettings("later_camelot_steps");
    const SongReport report = ConvertSong(rom, info, 1, opt, nullptr);
    SEQLIB_CHECK(report.ok && !report.silent);
    const Sf2Records sf = ReadSf2(report.sf2_path);
    const std::vector<uint8_t>& pcm = sf.chunks.at("smpl");
    const uint8_t* header = sf.Record("shdr", 46, 0);
    const uint32_t start = agblib::testing::agb_files::Le32(header + 20);
    SEQLIB_CHECK_EQ(agblib::testing::agb_files::Le32(header + 24) - start, 6);
    SEQLIB_CHECK_EQ(agblib::testing::agb_files::Le32(header + 36), 15768);
    bool sample_points = true;
    for (uint32_t i = 0; i < 6; i++)
    {
        sample_points = sample_points && int16_t(Le16(&pcm[2 * (start + i)])) == 256 * kPointsByHand[i];
    }

    SEQLIB_CHECK(sample_points);

    // The enhanced mode plays the decoded points: a point of 8 at the note's volume, and not the bytes of the steps.
    Sequencer enhanced(rom, info, 2);
    enhanced.StartMixing();
    enhanced.StartEnhancedMixing();
    const auto volume = [&](int side_volume)
    {
        return 8.0f * float(side_volume) * float(sounding[0].envelope) * (9.0f / 8.0f) / (512.0f * 128.0f);
    };
    const float point_right = volume(sounding[0].right);
    const float point_left = volume(sounding[0].left);

    // The echo keeps the console's levels: 32/128 of each side from 7 frames before, less 6/128 of the other side's.
    // The right side takes a quarter of itself from a frame before, and the left side a quarter of the right side from
    // a frame and a third before. The sample keeps its point, and the level stays flat across each third of a frame.
    using Thirds = std::array<float, 3>;
    const auto at = [](const std::vector<Thirds>& frames, int f, size_t third)
    {
        return f >= 0 ? frames[size_t(f)][third] : 0.0f;
    };
    std::vector<Thirds> rights;
    std::vector<Thirds> lefts;
    bool echoes = true;
    for (int f = 0; f < 15; f++)
    {
        enhanced.Step();
        const size_t samples = enhanced.EnhancedRight().size();
        Thirds right = {};
        Thirds left = {};
        for (size_t third = 0; third < 3; third++)
        {
            const float old_right = at(rights, f - 7, third);
            const float old_left = at(lefts, f - 7, third);
            const float last = at(rights, f - 1, third);
            const float tap = third == 0 ? at(rights, f - 2, 2) : at(rights, f - 1, third - 1);
            right[third] = point_right + (32.0f * old_right - 6.0f * old_left) / 128.0f + last / 4.0f;
            left[third] = point_left + (32.0f * old_left - 6.0f * old_right) / 128.0f + tap / 4.0f;
            const size_t middle = (2 * third + 1) * samples / 6;
            echoes = echoes && std::fabs(enhanced.EnhancedRight()[middle] - right[third]) < right[third] / 2000.0f &&
                     std::fabs(enhanced.EnhancedLeft()[middle] - left[third]) < left[third] / 2000.0f;
        }

        rights.push_back(right);
        lefts.push_back(left);
    }

    SEQLIB_CHECK(echoes);
}

// A later version of Camelot's sequencer gives the notes of a player whose priority is 120 a priority of 255, and
// others 254 at most. The search for a channel leaves out a released note of priority 255 (0x0824ecd0 and 0x0824ed3e in
// Mario Tennis: Power Tour).
void TestLaterCamelotPriority()
{
    Cart cart(kLaterCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    cart.WriteLaterCamelot(88, 32, -6);
    const uint32_t wave = cart.Wave(std::vector<int8_t>(4000, 20), 15768, 0);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, wave, 60, {255, 0, 255, 250})}});
    const auto notes = [&](uint8_t priority, uint8_t track_priority)
    {
        // Twelve notes take channels 2 to 11, then 1 and 0: the fourth is on channel 5. It releases, and one more note
        // starts.
        Track track;
        track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPrio, track_priority);
        for (int key = 60; key < 72; key++)
        {
            track.Tie(uint8_t(key), 127);
        }

        track.Eot(63).Tie(72, 127).Wait(96).Fine();
        cart.Song({track}, voices, 0, priority);
    };
    notes(120, 0);
    notes(100, 200);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // Every note has a priority of 255. The released note keeps its channel, and the last note takes the first in the
    // list of channels.
    Sequencer top(rom, info, 0);
    top.Step();
    SEQLIB_CHECK_EQ(top.GetChannel(2).priority, 255);
    SEQLIB_CHECK_EQ(top.GetChannel(5).midi_key, 63);
    SEQLIB_CHECK_EQ(top.GetChannel(0).midi_key, 72);

    // Every note has a priority of 254. The last note takes the released note's channel.
    Sequencer other(rom, info, 1);
    other.Step();
    SEQLIB_CHECK_EQ(other.GetChannel(2).priority, 254);
    SEQLIB_CHECK_EQ(other.GetChannel(5).midi_key, 72);
    SEQLIB_CHECK_EQ(other.GetChannel(0).midi_key, 71);
}

int RunTests()
{
    TestCamelotMixer();
    TestCamelotMixing();
    TestLaterCamelotMixing();
    TestLaterCamelotSteps();
    TestLaterCamelotPriority();

    return seqlib::testing::Summary("mp2k_camelot_mixer");
}

} // namespace
} // namespace agblib::sequence_export::mp2k

int main()
{
    return agblib::sequence_export::mp2k::RunTests();
}
