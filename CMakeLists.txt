# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(agblib VERSION 1.0.0 LANGUAGES C CXX)

# CMake 3.20 has no PROJECT_IS_TOP_LEVEL. agblib is the top-level project when the build starts in its folder.
if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(AGBLIB_TOP_LEVEL ON)
else()
    set(AGBLIB_TOP_LEVEL OFF)
endif()

option(AGBLIB_SEQUENCE_EXPORT "Build the conversions to MIDI, SoundFont 2 and SFZ. They require seqlib."
       ${AGBLIB_TOP_LEVEL})
option(AGBLIB_TESTS "Build agblib's tests and register them with CTest." ${AGBLIB_TOP_LEVEL})
option(AGBLIB_INSTALL "Install agblib's headers, libraries and CMake package. Requires installed dependencies." OFF)

if (AGBLIB_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ntdlib, and xsflib and seqlib with it. A project that uses agblib adds them first. A build of agblib alone adds the
# copies in externals/ntdlib, or finds installed ones. With AGBLIB_INSTALL, the copies are installed too.
if (NOT TARGET ntdlib::common)
    if (EXISTS ${PROJECT_SOURCE_DIR}/externals/ntdlib/CMakeLists.txt)
        if (NOT TARGET xsflib::common)
            set(XSFLIB_PSF ${AGBLIB_TOP_LEVEL})
            set(XSFLIB_INSTALL ${AGBLIB_INSTALL})
            add_subdirectory(externals/ntdlib/externals/xsflib)
        endif()
        set(NTDLIB_INSTALL ${AGBLIB_INSTALL})
        add_subdirectory(externals/ntdlib)
    else()
        find_package(ntdlib 4.0 REQUIRED)
    endif()
endif()

# seqlib, for the conversions. A build of agblib alone takes the copy that xsflib adds from its submodule, or finds an
# installed seqlib.
if (AGBLIB_SEQUENCE_EXPORT AND NOT TARGET seqlib::seqlib)
    find_package(seqlib 1.4 REQUIRED)
endif()

if (AGBLIB_TESTS)
    enable_testing()
endif()

# The builders of synthetic GBA cartridges for tests, beside ntdlib's and xsflib's test helpers.
add_library(agblib_testing INTERFACE)
add_library(agblib::testing ALIAS agblib_testing)
target_include_directories(agblib_testing INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                                                    $<INSTALL_INTERFACE:include>)
target_link_libraries(agblib_testing INTERFACE ntdlib::testing)
target_compile_features(agblib_testing INTERFACE cxx_std_20)
set_target_properties(agblib_testing PROPERTIES EXPORT_NAME testing)
set_property(GLOBAL APPEND PROPERTY AGBLIB_INSTALLED_TARGETS agblib_testing)

# The Game Boy Advance: the reader of ROMs and GSF files, the scanners of Thumb and ARM code that find a driver, the
# GBA's constants, the plan of a song's conversion from its tracks' loops and ends, and the steps that the drivers'
# cuts and joins share.
xsflib_add_project_library(agblib agb src/agb/arm.cpp src/agb/layout.cpp src/agb/rom.cpp src/agb/song_plan.cpp
                           src/agb/thumb.cpp)
target_link_libraries(agblib_agb PUBLIC xsflib::common xsflib::psf_container ntdlib::common)
xsflib_add_project_test(agblib agb agblib::agb)

# A model of the GBA's sound hardware: the PSG channels, the DirectSound FIFOs and the output stage.
xsflib_add_project_library(agblib agb_apu src/agb/apu.cpp)
target_link_libraries(agblib_agb_apu PUBLIC xsflib::common ntdlib::wave)
xsflib_add_project_test(agblib agb_apu agblib::agb_apu)

# MusicPlayer2000 (MP2K), the sound driver of Nintendo's GBA SDK: its detection, its songs and voice groups, the decoder
# and the encoder of its track commands in the form of ntdlib::commands, the cut and the join of a ROM's songs for an
# editor, and a model of its sequencer, which reads the commands through the decoder.
xsflib_add_project_library(agblib mp2k src/mp2k/commands.cpp src/mp2k/driver.cpp src/mp2k/layout.cpp
                           src/mp2k/sequencer.cpp src/mp2k/sequencer_enhanced.cpp src/mp2k/sequencer_mixer.cpp
                           src/mp2k/sequencer_psg.cpp src/mp2k/song.cpp)
target_link_libraries(agblib_mp2k PUBLIC agblib::agb ntdlib::commands PRIVATE ntdlib::wave)
xsflib_add_project_test(agblib mp2k_commands agblib::mp2k)
xsflib_add_project_test(agblib mp2k_layout agblib::mp2k)

# A player of MP2K's songs on the models of the driver and the GBA's sound hardware.
xsflib_add_project_library(agblib mp2k_player src/mp2k/archive_player.cpp)
target_link_libraries(agblib_mp2k_player PUBLIC agblib::mp2k agblib::agb_apu)

# Konami's GBA driver: detection, the commands of each revision, the decoder and the encoder of its track commands in
# the form of ntdlib::commands, the cut and the join of a ROM's songs for an editor, the sequencer, and the model of the
# output stage and the mixer.
xsflib_add_project_library(agblib konami src/konami/commands.cpp src/konami/driver.cpp src/konami/layout.cpp
                           src/konami/output.cpp src/konami/output_stages.cpp src/konami/output_mixer.cpp
                           src/konami/pitch.cpp src/konami/seqformat.cpp src/konami/sequencer.cpp
                           src/konami/sequencer_wct2004.cpp src/konami/sequencer_dungeon_dice.cpp)
target_link_libraries(agblib_konami PUBLIC agblib::agb ntdlib::commands PRIVATE ntdlib::wave)
# The output stage's files and the sequencer's build as one translation unit each. Their calls then stay inlined as
# before.
set_target_properties(agblib_konami PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
set_source_files_properties(src/konami/output.cpp src/konami/output_stages.cpp PROPERTIES UNITY_GROUP konami_output)
set_source_files_properties(src/konami/sequencer.cpp src/konami/sequencer_wct2004.cpp
                            src/konami/sequencer_dungeon_dice.cpp PROPERTIES UNITY_GROUP konami_sequencer)
xsflib_add_project_test(agblib konami_commands agblib::konami)
xsflib_add_project_test(agblib konami_layout agblib::konami)

# A song of a ROM with Konami's driver on the models of the driver and the sound hardware.
xsflib_add_project_library(agblib konami_player src/konami/archive_player.cpp)
target_link_libraries(agblib_konami_player PUBLIC agblib::konami agblib::agb_apu)

# Rare's GBA driver: detection, the tunes' commands and instruments, the decoder and the encoder of its track commands
# in the form of ntdlib::commands, the cut and the join of a ROM's tunes for an editor, and the model of the driver's
# per-frame routine.
xsflib_add_project_library(agblib rare src/rare/commands.cpp src/rare/driver.cpp src/rare/layout.cpp
                           src/rare/sequencer.cpp src/rare/song.cpp)
target_link_libraries(agblib_rare PUBLIC agblib::agb ntdlib::commands)
xsflib_add_project_test(agblib rare_commands agblib::rare)
xsflib_add_project_test(agblib rare_layout agblib::rare)

# A tune of a ROM with Rare's driver on the models of the driver and the sound hardware.
xsflib_add_project_library(agblib rare_player src/rare/archive_player.cpp)
target_link_libraries(agblib_rare_player PUBLIC agblib::rare agblib::agb_apu)

# Quintet's GBA driver: detection, the songs' commands, the decoder and the encoder of its channel commands in the form
# of ntdlib::commands, the cut and the join of a ROM's songs for an editor, and the model of the driver's play routine
# and its writes to the sound registers.
xsflib_add_project_library(agblib quintet src/quintet/commands.cpp src/quintet/driver.cpp src/quintet/instruments.cpp
                           src/quintet/layout.cpp src/quintet/sequencer.cpp src/quintet/sequencer_effects.cpp
                           src/quintet/sequencer_output.cpp src/quintet/song.cpp)
target_link_libraries(agblib_quintet PUBLIC agblib::agb ntdlib::commands)
# The parts of the sequencer build as one translation unit (sequencer_internal.h).
set_target_properties(agblib_quintet PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
set_source_files_properties(src/quintet/sequencer.cpp src/quintet/sequencer_effects.cpp src/quintet/sequencer_output.cpp
                            PROPERTIES UNITY_GROUP quintet_sequencer)
xsflib_add_project_test(agblib quintet_commands agblib::quintet)
xsflib_add_project_test(agblib quintet_layout agblib::quintet)
# quintet::ArchivePlayer: the model of the driver on the model of the GBA's sound hardware.
xsflib_add_project_library(agblib quintet_player src/quintet/archive_player.cpp)
target_link_libraries(agblib_quintet_player PUBLIC agblib::quintet agblib::agb_apu)

# Krawall, the GBA's player of XM and S3M modules: detection of its builds, the modules' patterns, their commands, the
# cut and join of a ROM, the instruments and samples, and the model of the player and its mixer.
xsflib_add_project_library(agblib krawall src/krawall/commands.cpp src/krawall/driver.cpp src/krawall/driver_songs.cpp
                           src/krawall/driver_files.cpp src/krawall/instruments.cpp src/krawall/layout.cpp
                           src/krawall/player.cpp src/krawall/player_effects.cpp src/krawall/player_mixer.cpp
                           src/krawall/player_records.cpp src/krawall/player_v1.cpp src/krawall/song.cpp
                           src/krawall/unpack.cpp)
target_link_libraries(agblib_krawall PUBLIC agblib::agb ntdlib::commands)
# The parts of the detection build as one translation unit (driver_internal.h).
set_target_properties(agblib_krawall PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
set_source_files_properties(src/krawall/driver.cpp src/krawall/driver_songs.cpp src/krawall/driver_files.cpp
                            PROPERTIES UNITY_GROUP krawall_driver)
xsflib_add_project_test(agblib krawall_layout agblib::krawall)

# Nintendo R&D2's GBA driver: detection, the commands of its revisions and the model of the driver.
xsflib_add_project_library(agblib rd2 src/rd2/commands.cpp src/rd2/driver.cpp src/rd2/layout.cpp src/rd2/output.cpp
                           src/rd2/sequencer.cpp src/rd2/sequencer_voices.cpp src/rd2/sequencer_levels.cpp
                           src/rd2/sequencer_output.cpp src/rd2/song.cpp)
target_link_libraries(agblib_rd2 PUBLIC agblib::agb ntdlib::commands PRIVATE ntdlib::wave)
# The parts of the sequencer build as one translation unit (sequencer_internal.h).
set_target_properties(agblib_rd2 PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
set_source_files_properties(src/rd2/sequencer.cpp src/rd2/sequencer_voices.cpp src/rd2/sequencer_levels.cpp
                            src/rd2/sequencer_output.cpp PROPERTIES UNITY_GROUP rd2_sequencer)
xsflib_add_project_test(agblib rd2_commands agblib::rd2)
xsflib_add_project_test(agblib rd2_layout agblib::rd2)

# Brownie Brown's GBA driver: detection, the commands of its two revisions and the model of the driver.
xsflib_add_project_library(agblib brownie src/brownie/commands.cpp src/brownie/driver.cpp src/brownie/layout.cpp
                           src/brownie/sequencer.cpp src/brownie/sequencer_channels.cpp src/brownie/sequencer_mix.cpp
                           src/brownie/song.cpp)
target_link_libraries(agblib_brownie PUBLIC agblib::agb ntdlib::commands PRIVATE ntdlib::wave)
# The parts of the sequencer build as one translation unit (sequencer_internal.h).
set_target_properties(agblib_brownie PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
set_source_files_properties(src/brownie/sequencer.cpp src/brownie/sequencer_channels.cpp src/brownie/sequencer_mix.cpp
                            PROPERTIES UNITY_GROUP brownie_sequencer)
xsflib_add_project_test(agblib brownie_commands agblib::brownie)
xsflib_add_project_test(agblib brownie_layout agblib::brownie)
xsflib_add_project_library(agblib brownie_game src/brownie/game_driver.cpp)
target_link_libraries(agblib_brownie_game PUBLIC agblib::brownie agblib::agb_system)
xsflib_add_project_library(agblib brownie_player src/brownie/archive_player.cpp)
target_link_libraries(agblib_brownie_player PUBLIC agblib::brownie agblib::agb_apu)

# Ubisoft Milan's GBA driver: detection and a model of its sequencer.
xsflib_add_project_library(agblib ubimilan src/ubimilan/commands.cpp src/ubimilan/driver.cpp src/ubimilan/layout.cpp
                           src/ubimilan/output.cpp src/ubimilan/sequencer.cpp src/ubimilan/song.cpp)
target_link_libraries(agblib_ubimilan PUBLIC agblib::agb ntdlib::commands PRIVATE ntdlib::wave)
xsflib_add_project_test(agblib ubimilan_commands agblib::ubimilan)
xsflib_add_project_test(agblib ubimilan_layout agblib::ubimilan)
xsflib_add_project_test(agblib ubimilan_output agblib::ubimilan)

# Nintendo R&D1's GBA driver for Standard MIDI Files: detection, the songs' records and files, the decoder
# and the encoder of the files' events in the form of ntdlib::commands, the cut and the join of a ROM's files for an
# editor, the instrument records and the model of a player's tracks and channels.
xsflib_add_project_library(agblib rd1 src/rd1/commands.cpp src/rd1/driver.cpp src/rd1/instruments.cpp
                           src/rd1/layout.cpp src/rd1/sequencer.cpp src/rd1/song.cpp)
target_link_libraries(agblib_rd1 PUBLIC agblib::agb ntdlib::commands)
xsflib_add_project_test(agblib rd1_commands agblib::rd1)
xsflib_add_project_test(agblib rd1_layout agblib::rd1)

# A piece of a ROM with Ubisoft Milan's driver on the models of the driver and the sound hardware.
xsflib_add_project_library(agblib ubimilan_player src/ubimilan/archive_player.cpp)
target_link_libraries(agblib_ubimilan_player PUBLIC agblib::ubimilan agblib::agb_apu)
xsflib_add_project_test(agblib ubimilan_player agblib::ubimilan_player)
xsflib_add_project_test(agblib ubimilan_enhanced agblib::ubimilan_player)

# AlphaDream's GBA driver: detection, the readers of its songs, instruments and samples, the decoder and the encoder
# of the songs' commands in the form of ntdlib::commands, and the cut and the join of a ROM's songs for an editor.
xsflib_add_project_library(agblib alphadream src/alphadream/commands.cpp src/alphadream/driver.cpp
                           src/alphadream/layout.cpp)
target_link_libraries(agblib_alphadream PUBLIC agblib::agb ntdlib::commands)
xsflib_add_project_test(agblib alphadream_commands agblib::alphadream)
xsflib_add_project_test(agblib alphadream_layout agblib::alphadream)

# Logik State's GBA driver, GBAModPlay: detection, the readers of its modules and banks of samples, models of the
# player of each format, and a module's listing.
xsflib_add_project_library(agblib logikstate src/logikstate/driver.cpp src/logikstate/mod1_pieces.cpp
                           src/logikstate/mod1_player.cpp src/logikstate/player.cpp src/logikstate/song.cpp
                           src/logikstate/mod30.cpp src/logikstate/mod30_player.cpp src/logikstate/mod30_games.cpp)
target_link_libraries(agblib_logikstate PUBLIC agblib::agb)

# Enough of a GBA to run a game's sound driver on the ARM interpreter: the memories, the I/O registers, the DMA's
# immediate transfers and the BIOS's calls that the drivers make. A game's MP2K driver runs on it (mp2k_game), as the
# reference for ntdlib's model of the driver.
xsflib_add_project_library(agblib agb_system src/agb/system.cpp)
target_link_libraries(agblib_agb_system PUBLIC agblib::agb ntdlib::arm)
xsflib_add_project_test(agblib agb_system agblib::agb_system)
xsflib_add_project_library(agblib mp2k_game src/mp2k/game_driver.cpp)
target_link_libraries(agblib_mp2k_game PUBLIC agblib::mp2k agblib::agb_system)
xsflib_add_project_library(agblib konami_game src/konami/game_driver.cpp)
target_link_libraries(agblib_konami_game PUBLIC agblib::konami agblib::agb_system)
xsflib_add_project_library(agblib rare_game src/rare/game_driver.cpp)
target_link_libraries(agblib_rare_game PUBLIC agblib::rare agblib::agb_system)
xsflib_add_project_library(agblib quintet_game src/quintet/game_driver.cpp)
target_link_libraries(agblib_quintet_game PUBLIC agblib::quintet agblib::agb_system)
xsflib_add_project_library(agblib ubimilan_game src/ubimilan/game_driver.cpp)
target_link_libraries(agblib_ubimilan_game PUBLIC agblib::ubimilan agblib::agb_system)
xsflib_add_project_test(agblib konami_output agblib::konami_game)
xsflib_add_project_library(agblib krawall_game src/krawall/game_driver.cpp)
target_link_libraries(agblib_krawall_game PUBLIC agblib::krawall agblib::agb_system)
# krawall::ArchivePlayer: a piece of a ROM with Krawall on the models of the player and the sound hardware.
xsflib_add_project_library(agblib krawall_player src/krawall/archive_player.cpp)
target_link_libraries(agblib_krawall_player PUBLIC agblib::krawall agblib::agb_apu)
xsflib_add_project_library(agblib rd1_game src/rd1/game_driver.cpp)
target_link_libraries(agblib_rd1_game PUBLIC agblib::rd1 agblib::agb_system)
# rd1::ArchivePlayer: a song of a ROM with R&D1's driver, on the game's copy of the driver and the model of the sound
# hardware.
xsflib_add_project_library(agblib rd1_player src/rd1/archive_player.cpp src/rd1/enhanced_mix.cpp)
target_link_libraries(agblib_rd1_player PUBLIC agblib::rd1_game agblib::agb_apu)
xsflib_add_project_test(agblib rd1_enhanced agblib::rd1_player)
if (AGBLIB_TESTS)
    # The test of the enhanced mode reads its private enhanced_mix.h.
    target_include_directories(agblib_rd1_enhanced_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
endif()
xsflib_add_project_library(agblib alphadream_game src/alphadream/game_driver.cpp)
target_link_libraries(agblib_alphadream_game PUBLIC agblib::alphadream agblib::agb_system)
# alphadream::ArchivePlayer: a song of a ROM with AlphaDream's driver, on the game's copy of the driver and the model of
# the sound hardware.
xsflib_add_project_library(agblib alphadream_player src/alphadream/archive_player.cpp src/alphadream/enhanced_mix.cpp)
target_link_libraries(agblib_alphadream_player PUBLIC agblib::alphadream_game agblib::agb_apu)
xsflib_add_project_test(agblib alphadream_enhanced agblib::alphadream_player)
if (AGBLIB_TESTS)
    # The test of the enhanced mode reads its private enhanced_mix.h.
    target_include_directories(agblib_alphadream_enhanced_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
endif()
xsflib_add_project_library(agblib rd2_game src/rd2/game_driver.cpp)
target_link_libraries(agblib_rd2_game PUBLIC agblib::rd2 agblib::agb_system)
xsflib_add_project_library(agblib rd2_player src/rd2/archive_player.cpp)
target_link_libraries(agblib_rd2_player PUBLIC agblib::rd2 agblib::agb_apu)
xsflib_add_project_test(agblib rd2_output agblib::rd2)
xsflib_add_project_test(agblib rd2_player agblib::rd2_player)
xsflib_add_project_test(agblib rd2_enhanced agblib::rd2_player)
xsflib_add_project_test(agblib konami_player agblib::konami_player)
xsflib_add_project_test(agblib konami_enhanced agblib::konami_player)
xsflib_add_project_test(agblib brownie_player agblib::brownie_player)

# The conversion of the GBA's songs to seqlib's MIDI, SoundFont 2 and SFZ files.
if (AGBLIB_SEQUENCE_EXPORT)
    # The GBA's conversions: what every driver's conversion shares (sequence_export_agb), MP2K's, Konami's, Rare's,
    # Quintet's, Krawall's, Nintendo R&D2's, Brownie Brown's, Ubisoft Milan's, AlphaDream's and Logik State's.
    xsflib_add_project_library(agblib sequence_export_agb
        src/sequence_export/agb/beat_grid.cpp
        src/sequence_export/agb/beat_midi.cpp
        src/sequence_export/agb/midi.cpp
        src/sequence_export/agb/module_bars.cpp
        src/sequence_export/agb/psg.cpp
        src/sequence_export/agb/settings.cpp
        src/sequence_export/agb/sf2.cpp
        src/sequence_export/agb/sfz.cpp
        src/sequence_export/agb/song_banks.cpp)
    target_link_libraries(agblib_sequence_export_agb PUBLIC seqlib::seqlib agblib::agb PRIVATE ntdlib::common)
    xsflib_add_project_library(agblib sequence_export_mp2k
        src/sequence_export/mp2k/convert.cpp
        src/sequence_export/mp2k/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_mp2k PUBLIC agblib::sequence_export_agb agblib::mp2k)
    xsflib_add_project_library(agblib sequence_export_konami
        src/sequence_export/konami/convert.cpp
        src/sequence_export/konami/performance.cpp
        src/sequence_export/konami/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_konami PUBLIC agblib::sequence_export_agb agblib::konami)
    xsflib_add_project_library(agblib sequence_export_rare
        src/sequence_export/rare/convert.cpp
        src/sequence_export/rare/performance.cpp
        src/sequence_export/rare/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_rare PUBLIC agblib::sequence_export_agb agblib::rare)
    xsflib_add_project_library(agblib sequence_export_quintet
        src/sequence_export/quintet/convert.cpp
        src/sequence_export/quintet/performance.cpp
        src/sequence_export/quintet/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_quintet PUBLIC agblib::sequence_export_agb agblib::quintet)
    xsflib_add_project_library(agblib sequence_export_krawall
        src/sequence_export/krawall/convert.cpp
        src/sequence_export/krawall/performance.cpp
        src/sequence_export/krawall/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_krawall PUBLIC agblib::sequence_export_agb agblib::krawall)
    xsflib_add_project_library(agblib sequence_export_rd2
        src/sequence_export/rd2/convert.cpp
        src/sequence_export/rd2/performance.cpp
        src/sequence_export/rd2/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_rd2 PUBLIC agblib::sequence_export_agb agblib::rd2)
    # AlphaDream's conversion: a model of the driver that plays a song frame by frame, the conversion to MIDI and
    # SoundFont 2, and a song's performance for an editor.
    xsflib_add_project_library(agblib sequence_export_alphadream
        src/sequence_export/alphadream/convert.cpp
        src/sequence_export/alphadream/performance.cpp
        src/sequence_export/alphadream/sequencer.cpp
        src/sequence_export/alphadream/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_alphadream PUBLIC agblib::sequence_export_agb agblib::alphadream)
    # Logik State's conversion: the model's playback of a module to MIDI and SoundFont 2.
    xsflib_add_project_library(agblib sequence_export_logikstate
        src/sequence_export/logikstate/convert.cpp
        src/sequence_export/logikstate/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_logikstate PUBLIC agblib::sequence_export_agb agblib::logikstate)
    xsflib_add_project_library(agblib sequence_export_brownie
        src/sequence_export/brownie/convert.cpp
        src/sequence_export/brownie/performance.cpp
        src/sequence_export/brownie/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_brownie PUBLIC agblib::sequence_export_agb agblib::brownie)
    xsflib_add_project_library(agblib sequence_export_ubimilan
        src/sequence_export/ubimilan/convert.cpp
        src/sequence_export/ubimilan/performance.cpp
        src/sequence_export/ubimilan/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_ubimilan PUBLIC agblib::sequence_export_agb agblib::ubimilan)
    xsflib_add_project_library(agblib sequence_export_rd1
        src/sequence_export/rd1/convert.cpp
        src/sequence_export/rd1/performance.cpp
        src/sequence_export/rd1/soundfont.cpp)
    target_link_libraries(agblib_sequence_export_rd1 PUBLIC agblib::sequence_export_agb agblib::rd1)
    xsflib_add_project_test(agblib sequence_export_agb agblib::sequence_export_agb)
    xsflib_add_project_test(agblib mp2k agblib::sequence_export_mp2k agblib::mp2k_player)
    xsflib_add_project_test(agblib mp2k_loops agblib::sequence_export_mp2k agblib::mp2k_player)
    xsflib_add_project_test(agblib mp2k_conversion agblib::sequence_export_mp2k agblib::mp2k_player)
    xsflib_add_project_test(agblib mp2k_mixer agblib::sequence_export_mp2k agblib::mp2k_player)
    xsflib_add_project_test(agblib mp2k_builds agblib::sequence_export_mp2k agblib::mp2k_player)
    xsflib_add_project_test(agblib mp2k_player agblib::sequence_export_mp2k agblib::mp2k_player)
    xsflib_add_project_test(agblib mp2k_enhanced agblib::sequence_export_mp2k agblib::mp2k_player)
    xsflib_add_project_test(agblib konami agblib::sequence_export_konami)
    xsflib_add_project_test(agblib konami_detection agblib::sequence_export_konami)
    xsflib_add_project_test(agblib konami_conversion agblib::sequence_export_konami)
    xsflib_add_project_test(agblib konami_revisions agblib::sequence_export_konami)
    xsflib_add_project_test(agblib konami_dungeon_dice agblib::sequence_export_konami)
    xsflib_add_project_test(agblib rare agblib::sequence_export_rare agblib::rare_player)
    xsflib_add_project_test(agblib rare_conversion agblib::sequence_export_rare agblib::rare_player)
    xsflib_add_project_test(agblib rare_player agblib::sequence_export_rare agblib::rare_player)
    xsflib_add_project_test(agblib quintet agblib::sequence_export_quintet agblib::quintet_player)
    xsflib_add_project_test(agblib quintet_performance agblib::sequence_export_quintet)
    xsflib_add_project_test(agblib krawall agblib::sequence_export_krawall agblib::krawall_player)
    xsflib_add_project_test(agblib rd1 agblib::sequence_export_rd1)
    xsflib_add_project_test(agblib rd2 agblib::sequence_export_rd2)
    xsflib_add_project_test(agblib rd2_levels agblib::sequence_export_rd2)
    xsflib_add_project_test(agblib rd2_performance agblib::sequence_export_rd2)
    xsflib_add_project_test(agblib brownie agblib::sequence_export_brownie)
    xsflib_add_project_test(agblib brownie_performance agblib::sequence_export_brownie)
    xsflib_add_project_test(agblib konami_performance agblib::sequence_export_konami)
    xsflib_add_project_test(agblib ubimilan agblib::sequence_export_ubimilan)
    xsflib_add_project_test(agblib ubimilan_performance agblib::sequence_export_ubimilan)
    xsflib_add_project_test(agblib alphadream agblib::sequence_export_alphadream)
    xsflib_add_project_test(agblib logikstate agblib::sequence_export_logikstate)
    xsflib_add_project_test(agblib logikstate_mod1 agblib::sequence_export_logikstate)
    xsflib_add_project_test(agblib logikstate_mod30 agblib::sequence_export_logikstate)
endif()

# The tests that take files supplied on the machine, from a local_tests.cmake in a folder of the source tree, if any.
if (AGBLIB_TESTS)
    xsflib_include_local_tests()
endif()

if (AGBLIB_INSTALL)
    xsflib_install_package(agblib)
endif()
