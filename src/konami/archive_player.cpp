// SPDX-License-Identifier: MIT

#include "agblib/konami/archive_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/output_peaks.h"
#include "agblib/agb/player_steps.h"
#include "agblib/agb/sound_registers.h"

namespace agblib::konami
{
namespace
{

// The bytes that a game stores in a FIFO after its reset.
constexpr std::array<int8_t, 4> kRestartBytes = {};

} // namespace

ArchivePlayer::ArchivePlayer(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
}

void ArchivePlayer::Start(int song, const ArchivePlayerOptions& options)
{
    if (options.store_cycle >= agb::kFrameCycles)
    {
        throw std::invalid_argument("the stores' cycle lies past the frame");
    }

    Request request;
    if (options.live && std::find(info_.live_songs.begin(), info_.live_songs.end(), song) != info_.live_songs.end())
    {
        request.setting = kLiveStageSetting;
        const auto table = info_.live_tables.find(song);
        if (table != info_.live_tables.end())
        {
            request.samples = table->second;
        }
    }

    Output output(rom_, info_);
    output.Play(song, request);
    if (!output.Song()->Valid())
    {
        throw std::invalid_argument("song " + std::to_string(song) + " can't be played");
    }

    output.SetEcho(options.echo);

    agb::Apu apu(output.SoundRegisters());
    apu.SetSequencer(options.sequencer_step, options.sequencer_cycles);

    // The init restarts the wave channel, and the registers alone start no channel. On most revisions, a note only
    // loads the wave RAM, flips the bank and sets the frequency. Without this store, the channel would never sound.
    apu.Store(agb::kRegSound3CntX, 2, 0x8000);

    // Up to the first stores, the FIFOs play the silence that the init left. A revision without a mixer has its timers
    // stopped there.
    const uint32_t period = output.FifoCycles(0);
    const uint64_t lead_cycles = uint64_t(output.FifoLead()) * period;
    if (period)
    {
        apu.SetTimer(0, period, period);
        const std::vector<int8_t> silence(size_t(options.store_cycle / period + output.FifoLead()), 0);
        apu.Queue(0, silence);
        apu.Queue(1, silence);
    }

    if (options.enhanced)
    {
        output.StartEnhancedMixing();
        apu.StartEnhanced();
        const uint64_t samples =
            ((options.store_cycle + lead_cycles) * agb::kEnhancedFrameSamples + agb::kFrameCycles) / agb::kFrameCycles;
        const std::vector<float> silence(size_t(samples), 0.0f);
        apu.QueueEnhanced(0, silence);
        apu.QueueEnhanced(1, silence);
    }

    output_.emplace(std::move(output));
    apu_ = std::move(apu);
    store_cycle_ = options.store_cycle;
    frames_ = 0;
    meter_frames_ = size_t(lead_cycles / agb::kFrameCycles);
    tail_frames_ = int(meter_frames_) + 2;
    tail_ = tail_frames_;
    enhanced_ = options.enhanced;
    changes_.clear();
    next_change_ = 0;
    mixed_cycles_ = 0;
    fifo_bytes_ = {};
    mixed_.clear();
    peaks_ = {};
    output_peaks_ = {};
}

const Output& ArchivePlayer::Driver() const
{
    agb::CheckStarted(bool(output_));

    return *output_;
}

void ArchivePlayer::RunFrame(std::vector<int16_t>& out)
{
    agb::CheckPlainFrame(enhanced_);

    Frame(&out, nullptr);
}

void ArchivePlayer::RunFrame(std::vector<float>& out)
{
    agb::CheckEnhancedFrame(bool(output_), enhanced_);

    Frame(nullptr, &out);
}

void ArchivePlayer::RunFrame()
{
    Frame(nullptr, nullptr);
}

uint16_t ArchivePlayer::SilentTracks() const
{
    return agb::SilentTracksOf(muted_, soloed_);
}

void ArchivePlayer::ApplyPsgMutes()
{
    const uint16_t silent = SilentTracks();
    uint8_t channels = 0;
    for (int c = 0; c < 4; c++)
    {
        if (silent >> output_->PsgChannelTrack(c) & 1)
        {
            channels = uint8_t(channels | 1u << c);
        }
    }

    apu_.SetMuted(channels);
}

void ArchivePlayer::RunHardware(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    agb::RunApuWithChanges(apu_, changes_, next_change_, mixed_cycles_, cycles, out, enhanced_out);
}

void ArchivePlayer::Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    agb::CheckStarted(bool(output_));

    const bool metering = metered_ && (out || enhanced_out);
    const size_t first_sample = out ? out->size() : enhanced_out ? enhanced_out->size() : 0;
    apu_.MeterChannels(metering);
    apu_.ClearPeaks();
    output_->MeterVoices(metering);
    output_->SetMutedTracks(SilentTracks());
    ApplyPsgMutes();
    RunHardware(store_cycle_, out, enhanced_out);

    output_->SetEnhancedFrames(enhanced_out != nullptr);
    const bool reached = output_->Frame();
    for (const agb::RegisterWrite& write : output_->SoundWrites())
    {
        apu_.Store(write.address, write.size, write.value);
    }

    for (int f = 0; f < 2; f++)
    {
        if (output_->FifoRestarted(f))
        {
            apu_.Queue(f, kRestartBytes);
        }
    }

    // The stores can give a channel to another track.
    ApplyPsgMutes();
    output_->Mix(fifo_bytes_[0], fifo_bytes_[1]);
    apu_.Queue(0, fifo_bytes_[0]);
    apu_.Queue(1, fifo_bytes_[1]);
    if (enhanced_)
    {
        apu_.QueueEnhanced(0, output_->EnhancedFifoA());
        apu_.QueueEnhanced(1, output_->EnhancedFifoB());
    }

    changes_ = output_->TimerChanges();
    next_change_ = 0;
    mixed_cycles_ = 0;
    RunHardware(agb::kFrameCycles - store_cycle_, out, enhanced_out);
    frames_++;

    const bool ended = !reached || !output_->Song()->AnyTrackActive();
    tail_ = !ended || !output_->MixerSilent() ? tail_frames_ : std::max(tail_ - 1, 0);

    peaks_ = {};
    output_peaks_ = {};
    if (!metering)
    {
        mixed_.clear();
        return;
    }

    TrackParts parts;
    for (size_t t = 0; t < parts.size(); t++)
    {
        parts[t] = output_->VoicePeaks(int(t));
    }

    mixed_.push_back(parts);
    if (mixed_.size() > meter_frames_)
    {
        TakePeaks(mixed_.front());
        mixed_.erase(mixed_.begin());
    }

    // The samples of the frame.
    agb::KeepOutputPeaks(output_peaks_, out, enhanced_out, first_sample);
}

void ArchivePlayer::TakePeaks(const TrackParts& parts)
{
    const uint8_t volumes = apu_.Register(agb::kRegSoundCntH);
    const uint8_t sides = apu_.Register(agb::kRegSoundCntHHigh);
    for (size_t t = 0; t < peaks_.size(); t++)
    {
        agb::KeepFifoPeak(peaks_[t], parts[t], volumes, sides);
    }

    // The stores of a frame can give a channel to a muted track. The channel can sound before the mutes follow. A muted
    // track's meter stays at 0.
    for (int c = 0; c < 4; c++)
    {
        const int track = output_->PsgChannelTrack(c);
        if (SilentTracks() >> track & 1)
        {
            continue;
        }

        const std::array<int32_t, 2> part = apu_.ChannelPeaks(c);
        agb::KeepPeak(peaks_[size_t(track)], part);
    }
}

bool ArchivePlayer::Busy() const
{
    return output_ && tail_ > 0;
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    agb::SetTrackBit(muted_, track, mute, kTracks);
}

void ArchivePlayer::SoloTrack(int track, bool solo)
{
    agb::SetTrackBit(soloed_, track, solo, kTracks);
}

void ArchivePlayer::MeterTracks(bool on)
{
    metered_ = on;
    if (!on)
    {
        mixed_.clear();
        peaks_ = {};
        output_peaks_ = {};
    }
}

ArchivePlayer::Snapshot ArchivePlayer::Save(const Snapshot*) const
{
    agb::CheckStarted(bool(output_));

    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.output_.emplace(*output_);
    snapshot.apu_ = apu_;
    snapshot.store_cycle_ = store_cycle_;
    snapshot.frames_ = frames_;
    snapshot.tail_ = tail_;
    snapshot.enhanced_ = enhanced_;
    snapshot.changes_ = changes_;
    snapshot.next_change_ = next_change_;
    snapshot.mixed_cycles_ = mixed_cycles_;
    return snapshot;
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    agb::CheckSnapshotOwner(snapshot.owner_ == this && snapshot.output_);

    output_.emplace(*snapshot.output_);
    apu_ = snapshot.apu_;
    store_cycle_ = snapshot.store_cycle_;
    frames_ = snapshot.frames_;
    tail_ = snapshot.tail_;
    enhanced_ = snapshot.enhanced_;
    changes_ = snapshot.changes_;
    next_change_ = snapshot.next_change_;
    mixed_cycles_ = snapshot.mixed_cycles_;
    fifo_bytes_ = {};
    mixed_.clear();
    peaks_ = {};
    output_peaks_ = {};
}

} // namespace agblib::konami
