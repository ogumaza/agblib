// SPDX-License-Identifier: MIT

// What the tests of the GBA's conversions share: the check of a conversion in memory against the files that a
// conversion writes.

#pragma once

#include "seqlib/testing/check.h"

#include "agblib/sequence_export/agb/settings.h"
#include "agblib/testing/agb_files.h"

namespace agblib::testing::agb_tests
{

// Checks a song's files in memory, `files`, from a driver's ConvertSongFiles(), against the files that its
// ConvertSong() wrote with the same settings, `written`: the same bytes and report, without paths.
template <typename Files>
void CheckFilesInMemory(const agblib::sequence_export::agb::SongReport& written, const Files& files)
{
    using agblib::testing::agb_files::ReadAll;

    SEQLIB_REQUIRE(written.ok && files.report.ok && !files.report.silent);
    SEQLIB_CHECK(!written.midi_path.empty() && !written.sf2_path.empty());
    SEQLIB_CHECK(files.report.midi_path.empty() && files.report.sf2_path.empty());
    SEQLIB_CHECK(!files.midi.empty() && files.midi == ReadAll(written.midi_path));
    SEQLIB_CHECK(!files.sf2.empty() && files.sf2 == ReadAll(written.sf2_path));
    SEQLIB_CHECK(files.report.warnings == written.warnings && files.report.seconds == written.seconds);
    SEQLIB_CHECK(files.report.loop_start == written.loop_start && files.report.loop_end == written.loop_end);
    SEQLIB_CHECK_EQ(files.report.tracks, written.tracks);
}

} // namespace agblib::testing::agb_tests
