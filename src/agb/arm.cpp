// SPDX-License-Identifier: MIT

#include "agblib/agb/arm.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include "agblib/agb/rom.h"
#include "code_match_internal.h"

namespace agblib::agb
{

bool MatchesArm(const Rom& rom, uint32_t at, std::span<const ArmPattern> pattern)
{
    return MatchesCode(rom, at, pattern);
}

uint32_t FindArm(const Rom& rom, uint32_t from, uint32_t to, std::span<const ArmPattern> pattern)
{
    for (uint32_t at = from; at + 4 * uint32_t(pattern.size()) <= to; at += 4)
    {
        if (MatchesArm(rom, at, pattern))
        {
            return at;
        }
    }

    return 0;
}

uint32_t ArmLiteral(const Rom& rom, uint32_t at)
{
    const uint32_t insn = rom.U32(at);
    const uint32_t offset = insn & 0xFFF;
    return rom.U32((insn & 0x00800000) ? at + 8 + offset : at + 8 - offset);
}

} // namespace agblib::agb
