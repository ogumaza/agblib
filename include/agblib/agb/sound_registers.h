// SPDX-License-Identifier: MIT

// The addresses of the GBA's sound registers, by GBATEK's names. kRegSound1CntL and kRegFifoA are in agb/gba.h. Each
// register's bytes also have the names of the Game Boy's registers (NR10 to NR52) where GBATEK gives them.

#pragma once

#include <cstdint>

#include "agblib/agb/gba.h"

namespace agblib::agb
{

// The PSG's channels: channel 1's sweep, duty and envelope, and frequency, then the same for channels 2 to 4.
constexpr uint32_t kRegSound1CntH = 0x04000062;
constexpr uint32_t kRegSound1CntX = 0x04000064;
constexpr uint32_t kRegSound2CntL = 0x04000068;
constexpr uint32_t kRegSound2CntH = 0x0400006C;
constexpr uint32_t kRegSound3CntL = 0x04000070;
constexpr uint32_t kRegSound3CntH = 0x04000072;
constexpr uint32_t kRegSound3CntX = 0x04000074;
constexpr uint32_t kRegSound4CntL = 0x04000078;
constexpr uint32_t kRegSound4CntH = 0x0400007C;

// The sound control registers, the wave RAM and FIFO B.
constexpr uint32_t kRegSoundCntL = 0x04000080;     // the PSG's volumes and sides
constexpr uint32_t kRegSoundCntH = 0x04000082;     // the DirectSound outputs' volumes and sides
constexpr uint32_t kRegSoundCntHHigh = 0x04000083; // SOUNDCNT_H's high byte: the FIFOs' sides, timers and resets
constexpr uint32_t kRegSoundCntX = 0x04000084;     // the master enable, and the PSG channels' flags
constexpr uint32_t kRegSoundBias = 0x04000088;
constexpr uint32_t kRegWaveRam = 0x04000090;
constexpr uint32_t kRegFifoB = 0x040000A4;

// The registers' bytes by the Game Boy's names.
constexpr uint32_t kRegNr10 = 0x04000060;
constexpr uint32_t kRegNr11 = 0x04000062;
constexpr uint32_t kRegNr12 = 0x04000063;
constexpr uint32_t kRegNr13 = 0x04000064;
constexpr uint32_t kRegNr14 = 0x04000065;
constexpr uint32_t kRegNr21 = 0x04000068;
constexpr uint32_t kRegNr22 = 0x04000069;
constexpr uint32_t kRegNr23 = 0x0400006C;
constexpr uint32_t kRegNr24 = 0x0400006D;
constexpr uint32_t kRegNr30 = 0x04000070;
constexpr uint32_t kRegNr31 = 0x04000072;
constexpr uint32_t kRegNr32 = 0x04000073;
constexpr uint32_t kRegNr33 = 0x04000074;
constexpr uint32_t kRegNr34 = 0x04000075;
constexpr uint32_t kRegNr41 = 0x04000078;
constexpr uint32_t kRegNr42 = 0x04000079;
constexpr uint32_t kRegNr43 = 0x0400007C;
constexpr uint32_t kRegNr44 = 0x0400007D;
constexpr uint32_t kRegNr50 = 0x04000080;
constexpr uint32_t kRegNr51 = 0x04000081;
constexpr uint32_t kRegNr52 = 0x04000084;

} // namespace agblib::agb
