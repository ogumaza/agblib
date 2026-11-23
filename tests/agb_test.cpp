// SPDX-License-Identifier: MIT

// Unit tests for the GBA's shared code: the reader of GSF files, the plan of a song's loop, the runner of Thumb
// routines and the lines of a trace.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "agblib/agb/files.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/song_plan.h"
#include "agblib/agb/thumb.h"
#include "agblib/agb/trace.h"
#include "agblib/testing/agb_cart.h"

namespace fs = std::filesystem;

namespace agblib::agb
{
using agblib::testing::agb_files::TempPath;

namespace
{

// Returns a zlib stream of `data` in stored blocks.
std::vector<uint8_t> ZlibStored(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do
    {
        const size_t n = std::min<size_t>(data.size() - pos, 0xFFFF);
        const bool last = pos + n == data.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), data.begin() + long(pos), data.begin() + long(pos + n));
        pos += n;
    } while (pos < data.size());

    uint32_t a = 1, b = 0;
    for (uint8_t c : data)
    {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }

    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8)
    {
        z.push_back(uint8_t(adler >> s));
    }

    return z;
}

// Writes a PSF file of `version` with `program`, the given tags and the reserved area.
void WritePsfFile(const fs::path& path, uint8_t version, const std::vector<uint8_t>& program, const std::string& tags,
                  const std::vector<uint8_t>& reserved = {})
{
    const std::vector<uint8_t> z = ZlibStored(program);
    std::vector<uint8_t> f = {'P', 'S', 'F', version};
    for (uint32_t v : {uint32_t(reserved.size()), uint32_t(z.size()), 0u})
    {
        for (int i = 0; i < 4; i++)
        {
            f.push_back(uint8_t(v >> (8 * i)));
        }
    }

    f.insert(f.end(), reserved.begin(), reserved.end());
    f.insert(f.end(), z.begin(), z.end());
    if (!tags.empty())
    {
        const std::string t = "[TAG]" + tags;
        f.insert(f.end(), t.begin(), t.end());
    }

    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
}

// Writes a GSF file whose program puts `data` at `address` and starts the CPU at `entry`, with the given tags and
// reserved area.
void WriteGsf(const fs::path& path, uint32_t address, const std::vector<uint8_t>& data, const std::string& tags,
              uint32_t entry = kRomBase, const std::vector<uint8_t>& reserved = {})
{
    std::vector<uint8_t> program;
    for (uint32_t v : {entry, address, uint32_t(data.size())})
    {
        for (int i = 0; i < 4; i++)
        {
            program.push_back(uint8_t(v >> (8 * i)));
        }
    }

    program.insert(program.end(), data.begin(), data.end());
    WritePsfFile(path, 0x22, program, tags, reserved);
}

void TestGsfLoading()
{
    // A GSF set in a folder whose name isn't ASCII: the library contains the cartridge, and the mini-GSF names it and
    // patches one byte.
    std::error_code ec;
    const fs::path dir = TempPath("gsf_\xC3\xA9\xE3\x81\x82");
    fs::create_directories(dir, ec);
    SEQLIB_CHECK(!ec);
    if (ec)
    {
        return;
    }

    std::vector<uint8_t> cart(0x1000, 0);
    const uint8_t kMarker[] = {0x78, 0x56, 0x34, 0x12};
    std::copy(std::begin(kMarker), std::end(kMarker), cart.begin() + 0x100);
    WriteGsf(dir / "set.gsflib", kRomBase, cart, "");
    WriteGsf(dir / "set-01.minigsf", kRomBase + 0xF00, {0x5A}, "title=Song 1\n_lib=set.gsflib\n");

    Rom rom;
    std::string err;

    SEQLIB_CHECK(rom.Load(Utf8(dir / "set-01.minigsf"), err));

    SEQLIB_CHECK(rom.FromGsf());
    SEQLIB_CHECK(rom.GsfLibrary() == Utf8(dir / "set.gsflib"));
    SEQLIB_CHECK_EQ(rom.U32(kRomBase + 0x100), 0x12345678); // from the library
    SEQLIB_CHECK_EQ(rom.U8(kRomBase + 0xF00), 0x5A);        // from the mini-GSF

    SEQLIB_CHECK(rom.Load(Utf8(dir / "set.gsflib"), err));

    SEQLIB_CHECK(rom.GsfLibrary().empty());

    // _lib2 goes on top of the file's program.
    WriteGsf(dir / "patch.gsflib", kRomBase + 0xF00, {0xA5}, "");
    WriteGsf(dir / "set-02.minigsf", kRomBase + 0xF00, {0x5A}, "_lib=set.gsflib\n_lib2=patch.gsflib\n");

    SEQLIB_CHECK(rom.Load(Utf8(dir / "set-02.minigsf"), err));

    SEQLIB_CHECK_EQ(rom.U8(kRomBase + 0xF00), 0xA5);
    SEQLIB_CHECK_EQ(rom.U32(kRomBase + 0x100), 0x12345678);

    // A _lib tag whose case differs from the library's name finds the library.
    WriteGsf(dir / "set-03.minigsf", kRomBase + 0xF00, {0x5A}, "_lib=SET.GSFLIB\n");

    SEQLIB_CHECK(rom.Load(Utf8(dir / "set-03.minigsf"), err));

    SEQLIB_CHECK_EQ(rom.U32(kRomBase + 0x100), 0x12345678);

    WriteGsf(dir / "orphan.minigsf", kRomBase + 0xF00, {0x5A}, "_lib=missing.gsflib\n");

    SEQLIB_CHECK(!rom.Load(Utf8(dir / "orphan.minigsf"), err));

    SEQLIB_CHECK(err == "the library missing.gsflib can't be read");

    // A folder dropped on the program by mistake.
    SEQLIB_CHECK(!rom.Load(Utf8(dir), err));

    SEQLIB_CHECK(err == "it's a folder, not a ROM or GSF file");

    // A zipped ROM. It has to be taken out of its archive first.
    std::vector<uint8_t> zip(0x200, 0);
    std::memcpy(zip.data(), "PK\x03\x04", 4);
    std::string write_error;
    SEQLIB_CHECK(WriteFile(Utf8(dir / "game.zip"), zip, write_error));

    SEQLIB_CHECK(!rom.Load(Utf8(dir / "game.zip"), err));

    SEQLIB_CHECK(err.find("it's an archive") == 0);

    fs::remove_all(dir, ec);
}

// A loaded GSF set gives where its CPU starts and the reserved area of each of its files.
void TestGsfEntryAndReserved()
{
    const xsflib::testing::TempFolder folder("gsf_entry");
    const fs::path& dir = folder.Path();

    // The library and the mini each have a reserved area, and each program its start. The mini's section at 0x09000004
    // lands 16 MiB into the image: sets of an image of 16 MiB or more have such sections.
    WriteGsf(dir / "set.gsflib", kRomBase, std::vector<uint8_t>(16, 0xAA), "", kRomBase, {1, 2, 3});
    WriteGsf(dir / "set-01.minigsf", 0x09000004, {1, 2, 3, 4}, "_lib=set.gsflib\n", 0x08000004, {9});
    Rom rom;
    std::string err;

    SEQLIB_CHECK(rom.Load(Utf8(dir / "set-01.minigsf"), err));

    // The library loads first: its start sets where the CPU starts, and its reserved area comes first.
    SEQLIB_CHECK_EQ(rom.GsfEntry(), kRomBase);
    SEQLIB_CHECK(rom.GsfReserved() == std::vector<std::vector<uint8_t>>({{1, 2, 3}, {9}}));
    SEQLIB_CHECK_EQ(rom.Size(), size_t{0x01000008});
    SEQLIB_CHECK_EQ(rom.U8(kRomBase + 0xF), 0xAA);
    SEQLIB_CHECK_EQ(rom.U32(0x09000004), 0x04030201);

    // A set that starts the CPU in the work memory loads with its start. A cartridge image has no reserved area and
    // starts at the cartridge.
    WriteGsf(dir / "work.gsf", kRomBase, {7}, "", 0x02000000);

    SEQLIB_CHECK(rom.Load(Utf8(dir / "work.gsf"), err));

    SEQLIB_CHECK_EQ(rom.GsfEntry(), 0x02000000);
    SEQLIB_CHECK(rom.GsfReserved().empty());

    std::string write_error;
    SEQLIB_CHECK(WriteFile(Utf8(dir / "game.gba"), std::vector<uint8_t>(0x200, 0), write_error));

    SEQLIB_CHECK(rom.Load(Utf8(dir / "game.gba"), err));

    SEQLIB_CHECK_EQ(rom.GsfEntry(), kRomBase);
    SEQLIB_CHECK(rom.GsfReserved().empty());
}

// The loader refuses a PSF file of another version, a program shorter than its header, a section that runs past the
// cartridge's 32 MiB and a section in the work memory (a multiboot set). An error in a library names the library.
void TestGsfErrors()
{
    const xsflib::testing::TempFolder folder("gsf_errors");
    const fs::path& dir = folder.Path();

    std::vector<uint8_t> other_program(12, 0);
    other_program[3] = 0x08;
    other_program[7] = 0x08;
    WritePsfFile(dir / "other.psf", 0x01, other_program, "");
    WritePsfFile(dir / "short.gsf", 0x22, std::vector<uint8_t>(11, 0), "");
    WriteGsf(dir / "past.gsf", kRomBase + 0x01FFFFFC, {1, 2, 3, 4, 5, 6, 7, 8}, "");
    WriteGsf(dir / "multiboot.gsf", 0x02000000, {1}, "", 0x02000000);
    WriteGsf(dir / "song.minigsf", kRomBase, {1}, "_lib=other.psf\n");
    Rom rom;
    std::string other_error;
    std::string short_error;
    std::string past_error;
    std::string multiboot_error;
    std::string library_error;

    const bool other = rom.Load(Utf8(dir / "other.psf"), other_error);
    const bool short_program = rom.Load(Utf8(dir / "short.gsf"), short_error);
    const bool past = rom.Load(Utf8(dir / "past.gsf"), past_error);
    const bool multiboot = rom.Load(Utf8(dir / "multiboot.gsf"), multiboot_error);
    const bool library = rom.Load(Utf8(dir / "song.minigsf"), library_error);

    SEQLIB_CHECK(!other && other_error == "it isn't a GSF (PSF version 0x22) file");
    SEQLIB_CHECK(!short_program && short_error == "its program: GSF program section is too short");
    SEQLIB_CHECK(!past && past_error == "its program: GSF program doesn't fit in the cartridge address space");
    SEQLIB_CHECK(!multiboot && multiboot_error == "its program: multiboot GSF sets (loaded to EWRAM) aren't supported");
    SEQLIB_CHECK(!library && library_error == "the library other.psf isn't a GSF (PSF version 0x22) file");
}

// A song's loop lasts until every track's loop is back where it started: loops of 3 and 4 bars make one of 12, and a
// loop that divides the longest leaves it as it is. A loop more than 8 times the longest isn't used: 72 ticks for loops
// of 8 and 9 is, but 90 for loops of 9 and 10 isn't, and the longest loop is the song's.
void TestLoopLength()
{
    SEQLIB_CHECK_EQ(LoopLength({7}), uint64_t(7));
    SEQLIB_CHECK_EQ(LoopLength({960, 480}), uint64_t(960));
    SEQLIB_CHECK_EQ(LoopLength({9216, 12288}), uint64_t(36864));
    SEQLIB_CHECK_EQ(LoopLength({8, 9}), uint64_t(72));
    SEQLIB_CHECK_EQ(LoopLength({9, 10}), uint64_t(10));
    SEQLIB_CHECK_EQ(LoopLength({6138, 6144}), uint64_t(6144));
}

// RunThumb() runs ldmia and stmia: one routine loads two words from the ROM with ldmia and adds them, and another
// stores its arguments on the stack with stmia and loads the second one back. A routine that stores a byte at the top
// of the address space gets no result. That byte isn't on the stack.
void TestRunThumb()
{
    // The first routine is ldr r1, =words; ldmia r1!, {r0, r2}; adds r0, r0, r2; bx lr, followed by the words' address,
    // 5 and 7. The second, at 0x14, is sub sp, #8; mov r3, sp; stmia r3!, {r0, r1}; ldr r0, [sp, #4]; add sp, #8;
    // bx lr. The third, at 0x20, is movs r0, #0; subs r0, #1; strb r0, [r0]; ldrb r0, [r0]; bx lr.
    Rom rom;
    rom.Assign({0x01, 0x49, 0x05, 0xC9, 0x80, 0x18, 0x70, 0x47, 0x0C, 0x00, 0x00, 0x08, 0x05, 0x00,
                0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x82, 0xB0, 0x6B, 0x46, 0x03, 0xC3, 0x01, 0x98,
                0x02, 0xB0, 0x70, 0x47, 0x00, 0x20, 0x01, 0x38, 0x00, 0x70, 0x00, 0x78, 0x70, 0x47});

    const std::optional<uint32_t> sum = RunThumb(rom, kRomBase, {0, 0, 0, 0});
    const std::optional<uint32_t> second = RunThumb(rom, kRomBase + 0x14, {3, 9, 0, 0});
    const std::optional<uint32_t> top = RunThumb(rom, kRomBase + 0x20, {0, 0, 0, 0});

    SEQLIB_CHECK(sum && *sum == 12);
    SEQLIB_CHECK(second && *second == 9);
    SEQLIB_CHECK(!top);
}

// A trace prints a record when it changes. A record that starts at zeros prints only once it changes. The trace stops
// when its frame returns false: here after three frames, though it may run five.
void TestTraceFrames()
{
    std::FILE* out = std::tmpfile();
    SEQLIB_REQUIRE(out != nullptr);
    int played = 0;
    const auto frame = [&]()
    {
        return ++played <= 3;
    };
    const auto records = [&]()
    {
        return std::vector<TraceRecord>{{"g", {uint8_t(played / 2)}}, {"c0", {0x12, 0x34}}};
    };

    TraceFrames(out, 5, frame, records);

    std::rewind(out);
    std::string text;
    for (int c = std::fgetc(out); c != EOF; c = std::fgetc(out))
    {
        text += char(c);
    }

    std::fclose(out);
    SEQLIB_CHECK(text == "0 c0 1234\n1 g 01\n");
    SEQLIB_CHECK_EQ(played, 4);
}

// Runs every test. Returns the exit code.
int RunTests()
{
    TestGsfLoading();
    TestGsfEntryAndReserved();
    TestGsfErrors();
    TestLoopLength();
    TestRunThumb();
    TestTraceFrames();

    return seqlib::testing::Summary("agb");
}

} // namespace
} // namespace agblib::agb

int main()
{
    return agblib::agb::RunTests();
}
