// SPDX-License-Identifier: MIT

#include "agblib/ite/driver.h"

#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/thumb.h"

namespace agblib::ite
{
namespace
{

using agblib::agb::Hex;

// The init copies two routines of ARM code to IWRAM with DMA 3. Its literal pool contains DMA3SAD, then the source,
// the destination and the control of each copy: 0x30 words for the first, 0x3A words for the second.
constexpr uint32_t kDma3Source = 0x040000D4;
constexpr uint32_t kFirstCopy = 0x84000030;
constexpr uint32_t kSecondCopy = 0x8400003A;

// How far before the pool the load of DMA3SAD can be, and how far before that load the init's push can be.
constexpr uint32_t kMaxInitPrologue = 0x80;
constexpr uint32_t kMaxPoolDistance = 0x400;

// How many halfwords before the game's call of the init the load of the bank can be.
constexpr uint32_t kMaxBankLoad = 8;

// The bank's limits that a read accepts.
constexpr int kMaxSongs = 255;
constexpr int kMaxSamples = 1024;

bool InIwram(uint32_t address)
{
    return (address >> 24) == 0x03;
}

// Whether the Thumb instruction at `at` is `ldr rX, [pc, #imm]`.
bool IsLiteralLoad(const Rom& rom, uint32_t at)
{
    return (rom.U16(at) & 0xF800) == 0x4800;
}

// The address of the init's literal pool, or 0 if the ROM has none.
uint32_t FindPool(const Rom& rom)
{
    for (uint32_t at = agblib::agb::kRomBase + 12; rom.Contains(at, 16); at += 4)
    {
        if (rom.U32(at) != kFirstCopy || rom.U32(at + 12) != kSecondCopy || rom.U32(at - 12) != kDma3Source)
        {
            continue;
        }

        if (rom.Contains(rom.U32(at - 8)) && InIwram(rom.U32(at - 4)) && rom.Contains(rom.U32(at + 4)) &&
            InIwram(rom.U32(at + 8)))
        {
            return at - 12;
        }
    }

    return 0;
}

// The init that loads DMA3SAD from the pool at `pool`: the push before the load. Returns 0 if there's none.
uint32_t FindInit(const Rom& rom, uint32_t pool)
{
    const uint32_t lowest =
        pool > agblib::agb::kRomBase + kMaxPoolDistance ? pool - kMaxPoolDistance : agblib::agb::kRomBase;
    for (uint32_t load = pool - 2; load >= lowest; load -= 2)
    {
        if (!IsLiteralLoad(rom, load) || agblib::agb::ThumbLiteralAddress(rom, load) != pool)
        {
            continue;
        }

        for (uint32_t at = load; at + kMaxInitPrologue > load && at >= agblib::agb::kRomBase; at -= 2)
        {
            if ((rom.U16(at) & 0xFF00) == 0xB500)
            {
                return at;
            }
        }

        return 0;
    }

    return 0;
}

// Reads the bank at `bank` into `info`. Returns false unless the header and every song's tables are in the ROM.
bool ReadBank(const Rom& rom, uint32_t bank, DriverInfo& info)
{
    if (!rom.Contains(bank, kBankSongTable))
    {
        return false;
    }

    const int songs = rom.U16(bank + kBankSongs);
    const int samples = rom.U16(bank + kBankSamples);
    if (songs == 0 || songs > kMaxSongs || samples > kMaxSamples || !rom.Contains(bank + kBankSongTable, 4 * songs) ||
        !rom.Contains(bank + rom.U32(bank + kBankSampleTable)))
    {
        return false;
    }

    for (int i = 0; i < songs; i++)
    {
        Song song;
        if (!ReadSong(rom, bank, i, song))
        {
            return false;
        }
    }

    info.bank = bank;
    info.song_count = songs;
    info.sample_count = samples;
    return true;
}

// One channel in the walk: the place in its track and the wait that the row routine reads, the bytes of the last event,
// and the command, parameter and volume that the tick routine uses. The bytes of an event stay in force until another
// event replaces them.
struct Channel
{
    uint32_t place = 0;
    uint8_t wait = 0;
    uint8_t flags = 0;
    uint8_t command = 0;
    uint8_t parameter = 0;
    uint8_t volume = 0;
    uint8_t event_command = 0;
    uint8_t event_parameter = 0;
    uint8_t event_volume = 0;
    uint8_t delay = 0;
};

// The walk's state across ticks: the order list's place, the rows left in the pattern, the speed and the tempo.
class Walk
{
public:
    // An entry of an order, and the samples of the mix before it.
    struct Entry
    {
        int order = 0;
        uint64_t samples = 0;
    };

    Walk(const Rom& rom, uint32_t bank, const Song& song) : rom_(rom), bank_(bank), song_(song)
    {
        channels_.resize(size_t(song.channels));
        speed_ = song.speed;
        tempo_ = song.tempo;
        next_tempo_ = song.tempo;
    }

    Flow Run(uint64_t max_samples)
    {
        Flow flow;
        uint64_t samples = 0;
        while (samples < max_samples && tempo_ > 0)
        {
            counter_ = uint8_t(counter_ - 1);
            if (counter_ == 0)
            {
                if (speed_ != 0)
                {
                    Row(samples, flow);
                }

                counter_ = uint8_t(speed_);
            }

            if (flow.kind == Flow::Kind::kLoop)
            {
                return flow;
            }

            Tick();
            samples += kTickSamples / uint32_t(tempo_);
            tempo_ = next_tempo_;
            if (speed_ == 0)
            {
                flow.kind = Flow::Kind::kEnd;
                flow.end = samples;
                return flow;
            }
        }

        flow.end = samples;
        return flow;
    }

private:
    // The row routine: a new pattern when the last one's rows are done, then each channel's next event where its wait
    // is over.
    void Row(uint64_t samples, Flow& flow)
    {
        rows_left_ = uint8_t(rows_left_ - 1);
        if (rows_left_ == 0xFF)
        {
            EnterOrder(samples, flow);
        }

        for (Channel& channel : channels_)
        {
            channel.wait = uint8_t(channel.wait - 1);
            if (channel.wait == 0xFF)
            {
                ReadEvent(channel, flow);
            }
        }
    }

    void EnterOrder(uint64_t samples, Flow& flow)
    {
        const int order = order_;
        const int pattern = rom_.U8(song_.order_list + uint32_t(order));
        rows_left_ = uint8_t(rom_.U8(song_.rows + uint32_t(pattern)) - 1);
        for (size_t i = 0; i < channels_.size(); i++)
        {
            const uint32_t entry = song_.tracks + 4 * (uint32_t(pattern) * uint32_t(song_.channels) + uint32_t(i));
            channels_[i].place = bank_ + rom_.U32(entry);
            channels_[i].wait = 0;
        }

        order_ = (order_ + 1) & 0xFF;
        if (order_ == song_.orders)
        {
            order_ = song_.restart;
        }

        // A loop: the song enters an order that it entered before, and no later in the list than the order before.
        if (!entries_.empty() && order <= entries_.back().order)
        {
            for (const Entry& entry : entries_)
            {
                if (entry.order == order)
                {
                    flow.kind = Flow::Kind::kLoop;
                    flow.loop_start = entry.samples;
                    flow.loop_end = samples;
                    flow.loop_order = order;
                }
            }
        }

        entries_.push_back({order, samples});
    }

    void ReadEvent(Channel& channel, Flow& flow)
    {
        uint32_t place = channel.place;
        channel.wait = rom_.U8(place);
        const uint8_t flags = rom_.U8(place + 1);
        place += 2;
        channel.flags = flags;
        if (flags & kEventNote)
        {
            place++;
            flow.notes++;
        }

        if (flags & kEventInstrument)
        {
            place++;
        }

        if (flags & kEventCommand)
        {
            channel.event_command = rom_.U8(place++);
        }

        if (flags & kEventParameter)
        {
            channel.event_parameter = rom_.U8(place++);
        }

        if (flags & kEventVolume)
        {
            channel.event_volume = rom_.U8(place++);
        }

        channel.place = place;

        // Command F acts in the row routine. An event with a parameter and no command reads the channel's command.
        const bool has_command = (flags & (kEventCommand | kEventParameter)) != 0;
        const uint8_t command = (flags & kEventCommand) ? channel.event_command : channel.command;
        if (has_command && command == kCommandSpeed)
        {
            if (channel.event_parameter > kMaxSpeed)
            {
                next_tempo_ = channel.event_parameter;
            }
            else
            {
                speed_ = channel.event_parameter;
            }

            channel.event_parameter = 0;
        }
    }

    // The tick routine: a channel takes the bytes of its event once the delay is over, and commands B and D move the
    // order list.
    void Tick()
    {
        for (Channel& channel : channels_)
        {
            TickChannel(channel);
        }
    }

    void TickChannel(Channel& channel)
    {
        uint8_t flags = channel.flags;
        if ((flags & kEventCommand) && channel.event_command == kCommandExtended &&
            (channel.event_parameter & 0xF0) == 0xD0)
        {
            flags &= uint8_t(~(kEventCommand | kEventParameter));
            channel.flags = flags;
            channel.delay = channel.event_parameter & 0x0F;
            channel.event_parameter = 0;
        }

        bool trigger = false;
        if (channel.delay == 0)
        {
            if (flags & kEventNote)
            {
                trigger = true;
                if (channel.command == kCommandVolume)
                {
                    channel.command = 0;
                    channel.parameter = 0;
                }

                channel.volume = 0;
            }

            if (flags & kEventVolume)
            {
                channel.volume = channel.event_volume;
            }

            if (flags & kEventParameter)
            {
                channel.parameter = channel.event_parameter;
            }

            if (flags & kEventCommand)
            {
                channel.command = channel.event_command;
            }

            if ((flags & kEventNote) && channel.command == kCommandPortamento)
            {
                trigger = false;
            }

            channel.flags = 0;
        }
        else
        {
            channel.delay--;
        }

        if (trigger && channel.command == kCommandOffset)
        {
            channel.command = 0;
            channel.parameter = 0;
            channel.event_parameter = 0;
        }

        if (channel.command == kCommandJump)
        {
            rows_left_ = 0;
            order_ = channel.parameter;
        }
        else if (channel.command == kCommandBreak)
        {
            rows_left_ = 0;
        }
        else if (channel.command == kCommandVolume && !(channel.volume >= 0x10 && channel.volume <= 0x4F))
        {
            channel.event_parameter = 0;
        }
    }

    const Rom& rom_;
    uint32_t bank_;
    const Song& song_;
    std::vector<Channel> channels_;
    std::vector<Entry> entries_;
    int order_ = 0;
    uint8_t rows_left_ = 0;
    uint8_t counter_ = 1;
    int speed_ = 0;
    int tempo_ = 0;
    int next_tempo_ = 0;
};

} // namespace

bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error)
{
    info = {};
    error.clear();
    const uint32_t pool = FindPool(rom);
    if (!pool)
    {
        return false;
    }

    info.init = FindInit(rom, pool);
    if (!info.init)
    {
        error = "ITE Media's driver: no init before its literal pool at " + Hex(pool);
        return false;
    }

    // The game loads the bank into r1 just before its call of the init.
    for (const uint32_t call : agblib::agb::FindCalls(rom, info.init))
    {
        for (uint32_t at = call - 2; at + 2 * kMaxBankLoad >= call; at -= 2)
        {
            if ((rom.U16(at) & 0xFF00) == 0x4900 && ReadBank(rom, agblib::agb::ThumbLiteral(rom, at), info))
            {
                info.init_call = call;
                info.log.push_back("ITE Media's driver: init at " + Hex(info.init) + ", called at " + Hex(call));
                info.log.push_back("bank at " + Hex(info.bank) + ": " + std::to_string(info.song_count) + " songs, " +
                                   std::to_string(info.sample_count) + " samples");
                return true;
            }
        }
    }

    error = "ITE Media's driver: no call of its init at " + Hex(info.init) + " passes a bank that can be read";
    return false;
}

bool ReadSong(const Rom& rom, uint32_t bank, int index, Song& song)
{
    song = {};
    if (index < 0 || index >= rom.U16(bank + kBankSongs))
    {
        return false;
    }

    const uint32_t header = bank + rom.U32(bank + kBankSongTable + 4 * uint32_t(index));
    if (!rom.Contains(header, kSongHeaderSize))
    {
        return false;
    }

    song.header = header;

    song.orders = rom.U16(song.header);
    song.restart = rom.U16(song.header + 2);
    song.channels = rom.U16(song.header + 4);
    song.speed = rom.U16(song.header + 6);
    song.tempo = rom.U16(song.header + 8);
    song.patterns = rom.U16(song.header + 10);
    if (song.channels > kMaxChannels)
    {
        song = {};
        return false;
    }

    const uint32_t track_count = uint32_t(song.patterns) * uint32_t(song.channels);
    song.tracks = song.header + kSongHeaderSize;
    song.order_list = song.tracks + 4 * track_count;
    song.rows = song.order_list + uint32_t(song.orders);
    if (!rom.Contains(song.tracks, 4 * track_count) ||
        !rom.Contains(song.order_list, uint32_t(song.orders + song.patterns)))
    {
        song = {};
        return false;
    }

    // The order list names patterns, and each track starts in the ROM.
    for (int order = 0; order < song.orders && song.patterns > 0; order++)
    {
        if (rom.U8(song.order_list + uint32_t(order)) >= song.patterns)
        {
            song = {};
            return false;
        }
    }

    for (uint32_t i = 0; i < track_count; i++)
    {
        if (!rom.Contains(bank + rom.U32(song.tracks + 4 * i), 2))
        {
            song = {};
            return false;
        }
    }

    return true;
}

Flow WalkSong(const Rom& rom, uint32_t bank, const Song& song, uint64_t max_samples)
{
    // A tempo over kTickSamples would make every tick 0 samples long.
    if (song.patterns == 0 || song.orders == 0 || song.channels == 0 || song.tempo <= 0 ||
        uint32_t(song.tempo) > kTickSamples)
    {
        return {};
    }

    Walk walk(rom, bank, song);
    return walk.Run(max_samples);
}

double FramesOfSamples(uint64_t samples)
{
    return double(samples) * kRingFrames / kRingSamples;
}

double SecondsOfSamples(uint64_t samples)
{
    return FramesOfSamples(samples) / agblib::agb::kFrameRate;
}

} // namespace agblib::ite
