# agblib

agblib is a C++20 library for the sound of the Game Boy Advance: the reader of ROMs and GSF sets, a model of the GBA's
sound hardware, the sound drivers of eleven families of games with a model of each, and the conversion of their songs
to MIDI, SoundFont 2 and SFZ files.

agblib builds on ntdlib, a library for the formats and hardware of Nintendo's consoles, and through it on xsflib and
seqlib. It uses ntdlib's ARM interpreter, its common form of a sequence's commands, its wave helpers and its rules for
cutting sound data into pieces for an editor. agblib's modules are static libraries. A project links the modules it
uses.

Each driver has a module named after it, with a player of its songs on the models (`agblib::<driver>_player`), a
copy of the game's driver that runs on the ARM interpreter as the reference for the model (`agblib::<driver>_game`),
and a conversion (`agblib::sequence_export_<driver>`), where the driver has them.

| Module | What it holds |
|---|---|
| `agblib::agb` | What the drivers of the Game Boy Advance share: the reader of ROMs and of GSF sets, the scanners of Thumb and ARM code that find a driver, the GBA's constants, and the plan of a song's conversion from its tracks' loops and ends |
| `agblib::agb_apu` | A model of the GBA's sound hardware: the Game Boy's four PSG channels, the two DirectSound FIFOs that the timers empty, and the output stage with its bias and resolution. The model runs on the CPU's cycles and takes the stores to the sound registers between runs. Its enhanced mixing mode puts out floats at about 48,022 Hz, without the output stage's limits: the PSG's channels through band-limited steps, and FIFOs of floats |
| `agblib::mp2k` | MusicPlayer2000 (MP2K), the sound driver of Nintendo's GBA SDK: its detection in a ROM, the readers of its songs and voice groups, and a model of its sequencer and its mixer. The model mixes the DirectSound channels point for point as the SDK's stereo and mono mixers do, with their reverb. It also mixes them as Camelot's mixer does, with its echo and its synth voices. Its enhanced mixing mode mixes the same channels, reverb and echo in floating point at about 48,022 Hz, through the band-limited interpolator. It makes the driver's writes to the PSG's registers too. Cut and Join cut a ROM's songs into pieces for an editor, by the rules of `ntdlib::sar_layout`, and lay them out again. A song that outgrows its place moves to the free space at the ROM's end |
| `agblib::mp2k_player` | A player of MP2K's songs on the models of the driver and the sound hardware (`agblib::mp2k` and `agblib::agb_apu`), a frame at a time, with frames without mixing, snapshots, mutes and meters of the tracks, the command behind each note, and the enhanced mixing mode. `agblib/mp2k/archive_source.h` gives xsflib's archive mode its steps |
| `agblib::konami` | Konami's GBA sound driver: its detection in a ROM, the reader of its songs, and a model of its sequencer, its output stage and its mixers. The model mixes the DirectSound voices point for point as each revision of the driver does, with its echo, and makes the driver's stores to the PSG's registers. The revision without a mixer runs on its FIFOs' timers. An enhanced mixing mode mixes the same voices and echo in floating point at about 48,022 Hz, through the band-limited interpolator. Cut and Join cut a ROM's songs into pieces for an editor, and lay them out again |
| `agblib::konami_player` | A player of the songs of Konami's driver on the models of the driver and the sound hardware (`agblib::konami` and `agblib::agb_apu`), a frame at a time, with frames without samples, snapshots, mutes and solos of the tracks, meters of the tracks and the output, and the enhanced mixing mode. `agblib/konami/archive_source.h` gives xsflib's archive mode its steps |
| `agblib::rare` | Rare's GBA sound driver: its detection in a ROM, the tunes' commands and instruments, the decoder and the encoder of its track commands in the form of `ntdlib::commands`, the cut and the join of a ROM's tunes for an editor, and a model of the routine that the driver runs each frame |
| `agblib::rare_player` | A player of the tunes of Rare's driver on the models of the driver and the sound hardware (`agblib::rare` and `agblib::agb_apu`), a frame at a time, with snapshots, mutes and meters of the tracks, and an enhanced mixing mode |
| `agblib::quintet` | Quintet's GBA sound driver: its detection in a ROM, the songs' commands and instruments, the decoder and the encoder of its channel commands in the form of `ntdlib::commands`, the cut and the join of a ROM's songs for an editor, and a model of the driver's play routine and its writes to the sound registers |
| `agblib::quintet_player` | A player of the songs of Quintet's driver on the models of the driver and the sound hardware (`agblib::quintet` and `agblib::agb_apu`), a frame at a time, with snapshots, mutes and meters of the tracks, and an enhanced mixing mode |
| `agblib::krawall` | Krawall, a player of XM and S3M modules for the GBA: the detection of its builds in a ROM, the modules' patterns, the decoder and the encoder of their commands, the cut and the join of a ROM for an editor, the instruments and samples, and a model of the player and its mixer |
| `agblib::krawall_player` | A player of a ROM's modules on the models of Krawall and the sound hardware (`agblib::krawall` and `agblib::agb_apu`), a frame at a time, with snapshots, mutes and meters of the tracks, and an enhanced mixing mode |
| `agblib::rd2` | Nintendo R&D2's GBA sound driver, the one of The Legend of Zelda: A Link to the Past & Four Swords and the Super Mario Advance games: its detection in a ROM, the reader of its songs, and a model of its sequencer and its mixer. The model mixes the sample voices point for point as each revision of the driver does, with its echo, and makes the driver's stores to the PSG's registers. An enhanced mixing mode mixes the same voices and echo in floating point at about 48,022 Hz, through the band-limited interpolator. `agblib/rd2/commands.h` decodes and encodes a sequence's commands, and `agblib/rd2/layout.h` cuts a ROM's sequences into pieces for an editor and lays them out again |
| `agblib::rd2_player` | A player of the songs of R&D2's driver on the models of the driver and the sound hardware (`agblib::rd2` and `agblib::agb_apu`), a frame at a time, with frames without samples, snapshots, mutes and solos of the tracks, meters of the tracks and the output, and the enhanced mixing mode. `agblib/rd2/archive_source.h` gives xsflib's archive mode its steps |
| `agblib::brownie` | Brownie Brown's GBA sound driver: its detection in a ROM, the reader of its sounds, and a model of its per-frame routine and of how its samples play. The Sword of Mana revision's model mixes its 5 voices point for point. The Magical Vacation revision's model plays a sample on each FIFO at its timer's rate. The model makes the driver's stores to the PSG's registers too. An enhanced mixing mode plays the same voices in floating point at about 48,022 Hz, through the band-limited interpolator. A decoder and an encoder cover the sounds' commands. Cut and Join cut a ROM's sounds into pieces for an editor, and lay them out again |
| `agblib::brownie_player` | A player of the sounds of Brownie Brown's driver on the models of the driver and the sound hardware (`agblib::brownie` and `agblib::agb_apu`), a frame at a time, with frames without samples, snapshots, mutes and solos of the tracks, meters of the tracks and the output, and the enhanced mixing mode. `agblib/brownie/archive_source.h` gives xsflib's archive mode its steps |
| `agblib::ubimilan` | Ubisoft Milan's GBA sound driver: its detection in a ROM, the reader of its pieces, and a model of its sequencer and its mixer. The model mixes the DirectSound voices point for point as the driver's mixer does, and makes the driver's stores to the PSG's registers. An enhanced mixing mode mixes the same voices in floating point at about 48,022 Hz, through the band-limited interpolator. Cut and Join cut a ROM's sequences into pieces for an editor, and lay them out again |
| `agblib::ubimilan_player` | A player of the pieces of Ubisoft Milan's driver on the models of the driver and the sound hardware (`agblib::ubimilan` and `agblib::agb_apu`), a frame at a time, with frames without samples, snapshots, mutes and solos of the tracks, meters of the tracks and the output, and the enhanced mixing mode. `agblib/ubimilan/archive_source.h` gives xsflib's archive mode its steps |
| `agblib::logikstate` | Logik State's GBA sound driver, a player of modules in the manner of ProTracker: its detection in a ROM, the reader of its modules and banks of samples, and a model of its rows, ticks, effects and mixer channels, a frame at a time. The model follows each channel's place in its sample as the driver's mixer does. It mixes no output. The GBAMOD30 modules of the later builds have a separate model, with each build's details as data, and a table of the games' settings |
| `agblib::ite` | ITE Media's GBA sound driver, the one of Hugo: The Evil Mirror, a player of 4-channel modules with ProTracker's command numbers in a bank of songs and samples: its detection in a ROM, the reader of its songs, and a walk of a song's orders, rows and commands B, D and F that gives the song's loop or end, as the driver's tick and row routines play them |
| `agblib::rd1` | Nintendo R&D1's GBA sound driver for Standard MIDI Files, the one of WarioWare, Inc.: Mega Microgame$! and Rhythm Tengoku: its detection in a ROM, the songs' records and files, the instrument records, and a model of a player's tracks and channels. `agblib/rd1/commands.h` decodes and encodes a file's events, and `agblib/rd1/layout.h` cuts a ROM's files into pieces for an editor and lays them out again |
| `agblib::rd1_player` | A player of the songs of R&D1's driver on the game's copy of the driver (`agblib::rd1_game`) and the model of the sound hardware (`agblib::agb_apu`), a frame at a time, with frames without samples, snapshots, auditions of a bank's programs, mutes and solos of the tracks, meters of the tracks and the output, and an enhanced mixing mode. The enhanced mode mixes the driver's voices, its echo and its filter in floating point at about 48,022 Hz, through the band-limited interpolator. `agblib/rd1/archive_source.h` gives xsflib's archive mode its steps |
| `agblib::alphadream` | AlphaDream's GBA sound driver, the one of Mario & Luigi: Superstar Saga: its detection in a ROM, in three versions, and the readers of its songs, instruments and samples. `agblib/alphadream/commands.h` decodes and encodes a song's commands, and `agblib/alphadream/layout.h` cuts a ROM's songs into pieces for an editor and lays them out again |
| `agblib::alphadream_player` | A player of the songs of AlphaDream's driver on the game's copy of the driver (`agblib::alphadream_game`) and the model of the sound hardware (`agblib::agb_apu`), a frame at a time, with frames without samples, snapshots, auditions of the instruments, mutes and solos of the tracks, meters of the tracks and the output, and an enhanced mixing mode. The enhanced mode mixes the driver's sample channels in floating point at about 48,022 Hz, through the band-limited interpolator. `agblib/alphadream/archive_source.h` gives xsflib's archive mode its steps |
| `agblib::agb_system` | Enough of a GBA to run a game's sound driver on ntdlib's ARM interpreter: the work memories, the cartridge, the I/O registers, the DMA's immediate transfers and the BIOS's calls of division, copying and LZ77 decompression. It links `agblib::agb` and `ntdlib::arm` |
| `agblib::mp2k_game` | A game's MP2K driver, run on `agblib::agb_system`: its init, its start of a song and its frames, with the mixer's output and the channels that each frame leaves. It's the reference for the model of the driver |
| `agblib::konami_game` | A game's copy of Konami's driver, run on `agblib::agb_system`: its init, its request of a song and its frames, with its stores to the sound registers and the bytes that its mixers give the FIFOs. It's the reference for the model of the driver |
| `agblib::rare_game` | A game's copy of Rare's driver, run on `agblib::agb_system`. It's the reference for the model of the driver |
| `agblib::quintet_game` | A game's copy of Quintet's driver, run on `agblib::agb_system`. It's the reference for the model of the driver |
| `agblib::krawall_game` | A game's copy of Krawall, run on `agblib::agb_system`. It's the reference for the model of the player |
| `agblib::rd2_game` | A game's copy of R&D2's driver, run on `agblib::agb_system`: its init, its request of a song and its frames, with its stores to the sound registers and the bytes that its mixer gives the FIFOs. It's the reference for the model of the driver |
| `agblib::brownie_game` | A game's copy of Brownie Brown's driver, run on `agblib::agb_system`: its init, its request of a sound and its frames, with its stores to the sound registers and the bytes that its interrupts give the FIFOs. It's the reference for the model of the driver |
| `agblib::ubimilan_game` | A game's copy of Ubisoft Milan's driver, run on `agblib::agb_system`: its start of a piece and its frames, with its stores to the sound registers and the bytes that its mixer gives the FIFOs. It's the reference for the model of the driver |
| `agblib::rd1_game` | A game's copy of R&D1's driver, run on `agblib::agb_system`: its init, the start of a song, the FIFOs' transfers and its frames, with its stores to the sound registers, the bytes that its mixer gives the FIFOs, and a hook before each frame's mix |
| `agblib::alphadream_game` | A game's copy of AlphaDream's driver, run on `agblib::agb_system`: the mixer's setup and the driver's init, the start of a song, VBlank's swap of the mixer's halves and its frames, with its stores to the sound registers, the halves that its mixer fills, the sample channels' records, its step routine, and a hook before each frame's mix |
| `agblib::sequence_export_agb` | What the conversions of the GBA's drivers share: the settings of a conversion and the names of its files, the SoundFont and MIDI writers on seqlib, the SFZ files made from a song's SoundFont, the banks of a song's programs, and the Game Boy PSG's voices. It links `agblib::agb` |
| `agblib::sequence_export_mp2k` | Conversion of MP2K's songs to MIDI and SoundFont 2 files. It links `agblib::sequence_export_agb` and `agblib::mp2k` |
| `agblib::sequence_export_konami` | Conversion of the songs of Konami's driver to MIDI and SoundFont 2 files, and in memory to SFZ files too. It links `agblib::sequence_export_agb` and `agblib::konami` |
| `agblib::sequence_export_rare` | Conversion of the tunes of Rare's driver to MIDI and SoundFont 2 files. It links `agblib::sequence_export_agb` and `agblib::rare` |
| `agblib::sequence_export_quintet` | Conversion of the songs of Quintet's driver to MIDI and SoundFont 2 files. It links `agblib::sequence_export_agb` and `agblib::quintet` |
| `agblib::sequence_export_krawall` | Conversion of Krawall's modules to MIDI and SoundFont 2 files. It links `agblib::sequence_export_agb` and `agblib::krawall` |
| `agblib::sequence_export_rd2` | Conversion of the songs of Nintendo R&D2's driver to MIDI and SoundFont 2 files. It links `agblib::sequence_export_agb` and `agblib::rd2` |
| `agblib::sequence_export_brownie` | Conversion of the sounds of Brownie Brown's driver to MIDI and SoundFont 2 files, and the record of a sound's performance: its stretches, the command behind each note and the changes of its mix. It links `agblib::sequence_export_agb` and `agblib::brownie` |
| `agblib::sequence_export_ubimilan` | Conversion of the pieces of Ubisoft Milan's driver to MIDI and SoundFont 2 files, and the record of a piece's performance, with the command behind each note. It links `agblib::sequence_export_agb` and `agblib::ubimilan` |
| `agblib::sequence_export_logikstate` | Conversion of the modules of Logik State's driver to MIDI and SoundFont 2 files. It links `agblib::sequence_export_agb` and `agblib::logikstate` |
| `agblib::sequence_export_rd1` | Conversion of the songs of Nintendo R&D1's driver to MIDI and SoundFont 2 files, and the songs' performances for an editor. It links `agblib::sequence_export_agb` and `agblib::rd1` |
| `agblib::sequence_export_alphadream` | Conversion of the songs of AlphaDream's driver to MIDI and SoundFont 2 files, on a model of the driver that plays a song frame by frame, and the record of a song's performance: its stretches, the command behind each note and the changes of its mix. It links `agblib::sequence_export_agb` and `agblib::alphadream` |

`agblib::testing` holds builders of small synthetic GBA cartridges with each driver's songs, for unit tests. It links
`ntdlib::testing`.

## Building

agblib requires CMake 3.20 or later, a C++20 compiler and ntdlib. ntdlib is a Git submodule at `externals/ntdlib`.
Clone with `git clone --recurse-submodules`, or run `git submodule update --init --recursive` in an existing clone.
The ntdlib submodule holds xsflib as a submodule, and xsflib holds seqlib.

agblib supports macOS on Apple Silicon and Windows x64 built with clang-cl. Other platforms and compilers aren't
supported.

- **Windows x64**: Visual Studio 2022 with the C++ workload and its "C++ Clang tools for Windows" component. Configure
  with `cmake -S . -B build -A x64 -T ClangCL` in place of the first command below.
- **macOS on Apple Silicon**: Xcode's command-line tools (`xcode-select --install`) and CMake (from cmake.org or
  Homebrew).

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Output is meant to be the same on both platforms. agblib builds with xsflib's flags. They turn off the contraction of
floating-point operations into fused multiply-adds. A project that includes agblib's headers should build its sources
the same way.

## Options

| Option | Default | Effect |
|---|---|---|
| `AGBLIB_SEQUENCE_EXPORT` | on when built alone | The `agblib::sequence_export_*` modules, the conversion to MIDI, SoundFont 2 and SFZ. Requires seqlib. |
| `AGBLIB_TESTS` | on when built alone | The tests, registered with CTest. |
| `AGBLIB_INSTALL` | off | Installs the headers, the libraries and the CMake package. A build of agblib alone installs its copies of ntdlib and xsflib too. Requires an installed seqlib. |

## Using agblib

Each module is a static library with an alias, such as `agblib::mp2k_player`. A project links the modules it uses.
The helpers for unit tests are `agblib::testing`.

A project that uses agblib adds xsflib and ntdlib first. agblib then uses those copies. The copy in agblib's
submodule goes unused.

As Git submodules:

```cmake
add_subdirectory(externals/xsflib EXCLUDE_FROM_ALL)
add_subdirectory(externals/ntdlib EXCLUDE_FROM_ALL)
add_subdirectory(externals/agblib EXCLUDE_FROM_ALL)
target_link_libraries(my_program PRIVATE agblib::mp2k_player)
```

From installed copies (`cmake --install build` after configuring each library with its `_INSTALL` option), with
xsflib and ntdlib installed first:

```cmake
find_package(agblib 1.0 REQUIRED)
target_link_libraries(my_program PRIVATE agblib::mp2k_player)
```

agblib's package finds ntdlib, and seqlib with `AGBLIB_SEQUENCE_EXPORT`. `tests/package` is a project that uses agblib
in each of these ways, to check the package.

## Credits

GSF was developed by CaitSith2 and Zoopd.

## Licence

agblib is under the MIT licence (`LICENSE`). It links ntdlib, xsflib and seqlib. They're under the MIT licence too.
ntdlib's `THIRD-PARTY-NOTICES.md` lists the third-party code in ntdlib's modules that agblib links.
