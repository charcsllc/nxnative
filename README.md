# nxnative

Play your own Nintendo Switch and Switch 2 games natively on Apple silicon Macs, iPhone and iPad. One script packs your game into a Mac app or an unsigned IPA that you sideload.

It is not an emulator: the game's own code runs directly on the Apple chip, with Switch graphics rebuilt on Metal. Nothing from any game is in this repository, and the project is not affiliated with Nintendo or any game publisher.

[Games](#games) · [Community front ends](#community-front-ends) · [Requirements](#requirements) · [How to play](#how-to-play) · [Windows and Linux](#windows-and-linux) · [Discussions](#discussions) · [For developers](#for-developers)

**New: Windows and Linux PCs** (x86-64, in progress): the same games packed for a PC and played from a launcher window, with graphics on Vulkan. One game is tested so far. See [Windows and Linux](#windows-and-linux).

## Games

The full [tested games table](dm/games/README.md#games) includes original Switch games, tested builds, engines and graphics APIs. Each game's note records its tested platforms, progress and known issues.

These Switch 2 games have passed Mac gameplay testing. Build them from a [`.nup` file or a dump folder](#switch-2-games):

- [Donkey Kong Bananza](dm/games/040074A01BF12000/README.md)
- [Fast Fusion](dm/games/040041E0212AE000/README.md)
- [Fire Emblem Fortune's Weave](dm/games/04007EF01F8AE000/README.md) — up to 120 fps
- [Hyrule Warriors: Age of Imprisonment](dm/games/04003F40232DE000/README.md)
- [Kirby Air Riders](dm/games/040077201BE50000/README.md)
- [Mario Kart World](dm/games/0400C3F00006E000/README.md)
- [Mario Tennis Fever](dm/games/0400D4201C6EA000/README.md)
- [Nintendo Switch 2 Welcome Tour](dm/games/040057C02159A000/README.md) — Area A tested; exhibits need further testing
- [Nintendo Switch Sports Resort](dm/games/04008E001F098000/README.md)
- [Orbitals](dm/games/040032B023CA4000/README.md) — two players: a controller for player 2
- [Pokémon Pokopia](dm/games/0400A3A01EE4C000/README.md)
- [PRAGMATA](dm/games/0400C76024710000/README.md)
- [Splatoon Raiders](dm/games/0400380020056000/README.md)
- [Star Fox](dm/games/0400E88025B1A000/README.md)
- [Super Mario Bros. Wonder – Nintendo Switch 2 Edition + Meetup in Bellabel Park](dm/games/040015100B514001/README.md)
- [The Legend of Zelda: Ocarina of Time](dm/games/0400486024DEC000/README.md)
- [Yoshi and the Mysterious Book](dm/games/0400245021D2C000/README.md)

## Community front ends

Community projects build on nxnative with tools to turn your games into apps for your iPhone, iPad or Mac without the command line, and libraries to organize and launch them. They are separate projects: questions and bugs about them go to their own pages.

- [NX2Apple](https://github.com/LNPANDY/NX2Apple) — Mac builder with a shared iPhone/iPad game library.
- [NX Play](https://github.com/varyform/nx-native-wrapper) — Mac library with device installs and save sync.
- [NxNative Launcher](https://github.com/rennan101us/nxnative_Launcher) — Mac launcher with guided builds and controller mapping.
- [nxnative Easy](https://github.com/nalancer08/nxnative-easy) — Mac game library with guided builds, iPhone and iPad installation and signature renewal, and iCloud save sync.
- [LumaNX](https://github.com/Xc1Ym/LumaNX) — Mac game builder with IPA export.
- [GameCase](https://github.com/Tiaoge-tou/GameCase) — iOS game library based on LiveContainer.

## Requirements

- **Mac:** Apple silicon, macOS 26.6 or later. Building games requires Xcode 27.2.
- **iPhone:** A17 Pro chip or later, with iOS 26 or later.
- **iPad:** M1 chip or later, with iPadOS 26 or later.

The games' textures are BC-compressed, which older iPhones cannot read. Switch 2 builds are tested on the Mac; check each game's note for device-specific validation.

For a game without a profile, the script writes a survey and tries a provisional build with guessed settings. That build may not start; see [Port another game](docs/development.md#port-another-game).

## How to play

For a Switch 2 game, follow the [Switch 2 build instructions](#switch-2-games) with a `.nup` file or a dump folder.

### Build from the command line

You need a Mac with macOS 26.6 or later, [Xcode](https://developer.apple.com/download/all/?q=Xcode%2027.2) 27.2, [Homebrew](https://brew.sh) and an arm64 build of Python 3.12 or newer. To build an IPA, install Xcode's iOS platform (Xcode → Settings → Components).

The shaders are built with Xcode 27.2's Metal compiler. Select it before step 1: `sudo xcode-select -s /Applications/Xcode.app`. With several Xcode versions installed, name this one `Xcode-27.2.app` and select it with `sudo xcode-select -s /Applications/Xcode-27.2.app`.

**1. Install the tools** (only once), in Terminal:

```
xcodebuild -downloadComponent MetalToolchain
git clone https://github.com/seedds/nxnative.git && cd nxnative
brew install xcodegen zstd glslang
python3 -m pip install lz4 "capstone>=5,<6" fonttools nsz zbic "backports.zstd; python_version < '3.14'"
```

**2. Put your Switch 1 game in a folder.** Include any updates and DLC you want to pack. The script reads `.nsp`, `.xci`, `.nsz` and `.xcz` files.

```
~/Games/MarioWonder/
  Super Mario Bros. Wonder [010015100B514000][v0].nsp
  Super Mario Bros. Wonder [010015100B514800][v327680].nsp
```

**3. Build**, with your game folder and your `prod.keys`:

```
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys
```

For a Mac app, add `--platform mac-app` and open the resulting `.app` in Finder. By default, the script saves the IPA or Mac app in the game's folder. See [The packing script in detail](docs/development.md#the-packing-script-in-detail) for all options.

The first build takes 30–60 minutes and needs about twice the game's size plus 10 GB of free disk space. Rebuilding is much faster.

### Install

Install the IPA on your iPhone or iPad with a sideloading tool such as AltStore, Sideloadly or LiveContainer. If your tool offers **Increased Memory Limit**, turn it on: the bigger games need it.

Game too big to install? Build again with `--romfs separate`, and run the command the script prints once to copy the game files to your phone ([Quick reinstalls](docs/development.md#quick-reinstalls)). With AltServer, also add `--shader-files`: it cannot install a file of 2 GiB or more.

If a sideloading tool fails to extract a large IPA, try `--prepare-xcode` and [install from Xcode](docs/development.md#build-and-run-on-a-device-with-xcode).

Controls, saves and settings: [Playing](docs/playing.md).

### Switch 2 games

Build a Switch 2 game from one [NUP](https://github.com/a55uka/nup) container (a `.nup` file) or a dump folder. Both inputs work without `prod.keys`. If the game has no profile in this project, the script surveys it and creates a provisional profile. The resulting build may fail to boot. See [Port another game](docs/development.md#port-another-game).

**From a `.nup` file**, which holds the base game, its updates and DLC in one file. Give the file, or a folder that holds it:

```
python3 scripts/build_ipa_from_nsp.py ~/Games/DonkeyKongBananza.nup --platform mac-app
```

[Hyrule Warriors: Age of Imprisonment](dm/games/04003F40232DE000/README.md) is played this way: its 47 GB NUP unpacks in under five minutes. A NUP that carries the game's control data (its NACP) and icon gives the app the game's name and icon; otherwise the name comes from the game's profile and the app has no icon unless you give it one with `--icon <image>` (`ICON` in Spyder; a JPEG or PNG of any size). The script also reads [NUPs with shifted metadata numbers](docs/reference/packing.md#a-nup-icon-is-written-as-control-data).

A Nintendo Switch 2 Edition of a Switch 1 game can hold both of its programs in one NUP: the Switch 1 program with the game data, and the Switch 2 program, which reads that data from the Switch 1 program and its Switch 2 files from the upgrade add-on. The script packs the Switch 2 program, with the Switch 1 program's game files beside its own and the add-on as DLC; `--program 0` packs the Switch 1 program instead.

A NUP without the base game can have an empty base record or omit the base record entirely. With no base record, every update must be marked `standalone_update`. In either case, the script reports that it is using the selected update as the whole game. That update needs a complete ExeFS and RomFS. If files are missing, pack a NUP with the base game. The script selects the newest update by default. Use `--package v<N>` to select another update. `v0` requires a base game.

**From a folder of files**. The script expects this layout (`updates` and `dlc` are optional):

```
~/Games/DonkeyKongBananza/
  base/exefs/                     main, main.npdm, sdk, rtld
  base/romfs/
  updates/458752/exefs/           the update's version, as a decimal number
  updates/458752/romfs/           the patched RomFS, not a patch
  dlc/040074a01bf13001/0/romfs/   the DLC's title ID
```

```
python3 scripts/build_ipa_from_nsp.py ~/Games/DonkeyKongBananza --platform mac-app
```

You can also pass a complete update folder directly, for example `~/Games/DonkeyKongBananza/updates/458752`. The script treats a folder with `exefs/` and `romfs/` at its root as `base/`, including folders made by `nup extract`. Use `--icon <image>` (`ICON` in Spyder) to supply an icon for the app. To turn a dump folder into a NUP, run `nup pack ~/Games/DonkeyKongBananza game.nup`. See [The packing script in detail](docs/development.md#the-packing-script-in-detail) for more options.

## Windows and Linux

The port runs the games on x86-64 PCs with Windows 10 or 11, or Linux. A PC's processor is not ARM, so the game's code cannot run unchanged as on Apple chips. When you pack a game, its code is compiled once, ahead of time, to native x86-64 code (`nxaot`, built on LLVM). A JIT fallback (dynarmic) runs any code that was not found ahead of time. The graphics go to Vulkan instead of Metal, and the window, controllers and sound to SDL3. As on Apple, nothing from any game is in the download: you pack your own dump with your own keys, on your PC.

**Status.** One game is tested so far: **Mario vs. Donkey Kong**. It was played on Linux with a software renderer (every level visited), and packed and played on a real Windows 11 PC with an NVIDIA RTX 4060. Other games are not tested yet. On Linux, movies are black and silent until a video decoder is chosen; on Windows they play through Media Foundation. Progress and open items: [docs/design/migration-status.md](docs/design/migration-status.md).

### What your PC needs

- A 64-bit Intel or AMD processor.
- A GPU driver with **Vulkan 1.3** (current NVIDIA, AMD and Intel drivers have it).
- **Windows 10 or 11.** The N editions also need the Media Feature Pack for the movies (Settings, Apps, Optional features).
- **Linux:** glibc 2.38 and GCC 13's C++ library or later (Ubuntu 24.04 or later, Debian 13), and the Vulkan loader (`libvulkan1` on Ubuntu and Debian, which the GPU driver packages bring).
- Free disk space on the game's drive: the game's size plus about 7 GB.
- Your own game dump (NSP or XCI, with its update and DLC if you have them) and your console's `prod.keys`.

Nothing else is installed. The download is one self-contained folder, the **bundle**. It carries the launcher, the host program, the recompiler, its own Python 3.14 with the packer's modules, and the extraction and shader tools. There is no installer: unpack it anywhere and run it from there.

### Get the bundle

| System | File | Open |
|---|---|---|
| Windows | `nxnative-windows-x86_64.zip` | `nxlauncher.exe` |
| Linux | `nxnative-linux-x86_64.tar.gz` | `nxlauncher` |

Download them from [Releases](https://github.com/charcsllc/nxnative/releases) (the latest `port-preview` release lists each file's SHA-256). The [port workflow](.github/workflows/port.yml) is set to build both on every push and on a manual run. To build them yourself, see [Build the bundles](#build-the-bundles) below. A newer bundle can be unpacked anywhere: its launcher takes over the games packed with an older one.

### Pack and play (Windows)

1. Unzip `nxnative-windows-x86_64.zip`, for example to `C:\nxnative\`.
2. Put each game in a folder of its own, outside the bundle's folder, for example `C:\Games\MarioVsDK\Mario vs. Donkey Kong [0100B99019412000][v0].xci`. The `[title ID][version]` in the name helps the packer; without it, the packer reads the title from the file.
3. Put `prod.keys` in `%USERPROFILE%\.switch\prod.keys`, or choose it later with **Choose prod.keys...**.
4. Open `nxlauncher.exe`. The bundle is not signed, so Windows may show "Windows protected your PC": choose **More info**, then **Run anyway**.
5. Choose **Add game...**, pick the game's `.nsp` or `.xci`, check the keys it found, and choose **Pack**. The window shows each step with a progress bar. The first pack takes minutes, most of them compiling the game's code; packing the same game again takes under a minute.
6. Choose the game in the list and **Play**. To end it, close the game's window or choose **Stop** in the launcher; both keep what the game saved.

The **Saves** tab backs up and restores the game's save data. **Games folder** (with **Browse...**) is the folder the launcher lists games from. Every setting: [On Windows and Linux](docs/settings.md#on-windows-and-linux). The game's log is `<game folder>\<title ID>-work\run\log.txt`.

Without the launcher, from a terminal: `pack.cmd "C:\Games\MarioVsDK" -k "%USERPROFILE%\.switch\prod.keys"`, then `C:\Games\MarioVsDK\<title ID>_build\run.cmd`.

### Pack and play (Linux)

```
tar -xzf nxnative-linux-x86_64.tar.gz
./nxnative-linux-x86_64/nxlauncher
```

Then follow the same steps as on Windows: **Add game...**, **Pack**, **Play**. `prod.keys` goes in `~/.switch/prod.keys`. The file dialog uses the desktop's portal, or `zenity`. From a terminal: `./nxnative-linux-x86_64/pack.sh ~/Games/MarioVsDK -k ~/.switch/prod.keys`, then `~/Games/MarioVsDK/<title ID>_build/run.sh`.

The Linux build ships as a `.tar.gz` folder that runs from where it is unpacked, the same on every distribution that meets the requirements above. No `.deb` or other system package is made.

### Controls

- **Controllers:** any controller SDL3 knows works, including Xbox, PlayStation (with gyro), the Switch Pro Controller and handheld PCs' built-in controllers.
- **Keyboard:** the same keys as on the Mac.
- **Mouse:** stands in for the touchscreen.
- **Rumble:** works where the controller has it.

### Build the bundles

Everything builds on Linux with Docker; on Windows, use Docker Desktop with WSL2. The Windows programs (`.exe`) are cross-built on Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), so no Visual Studio is needed. From the repository root:

```sh
docker build -t nxnative-build -f docker/Dockerfile.build docker

# Linux: nxhost-run and nxlauncher into build/linux
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh -c \
  'cmake -S dm/host -B build/linux -G Ninja -DGAME=010015100B514000 -DNXHOST_PROGRAM=ON -DNXHOST_LAUNCHER=ON -DNXHOST_JIT=ON && cmake --build build/linux --target nxhost-run nxlauncher'

# Windows: nxhost-run.exe and nxlauncher.exe into build/win64
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src mstorsjo/llvm-mingw:20261020 sh -c \
  'cmake -S dm/host -B build/win64 -G Ninja -DGAME=010015100B514000 -DCMAKE_TOOLCHAIN_FILE=/src/dm/host/cmake/x86_64-w64-mingw32.cmake -DNXHOST_PROGRAM=ON -DNXHOST_LAUNCHER=ON -DNXHOST_JIT=ON && cmake --build build/win64 --target nxhost-run nxlauncher && sh dm/tools/mingw_licences.sh build/win64'
```

A complete bundle needs more than these two programs. It also takes the recompiler (`nxaot`, and `nxaot.exe` with LLVM cross-built for Windows), the published C# tools (`nx_extract`, the Vulkan shader translator), the bundled Python and the SPIR-V tools. Each comes from a script in Docker, and every download those scripts make is pinned by SHA-256. [scripts/package_port.py](scripts/package_port.py) then puts the bundle together and checks it (`--archive --check` writes the `.zip` or `.tar.gz`).

The steps and their commands are in [docker/README.md](docker/README.md) (images, the port's tools, the recompiler) and [docs/development.md, The port's bundles](docs/development.md#the-ports-bundles). The [port workflow](.github/workflows/port.yml) runs the same steps on GitHub Actions. The host's tests run with CTest in the same image (`ctest --test-dir build/linux`). The Windows tests run on a Windows PC or under Wine ([docker/README.md](docker/README.md#running-the-windows-tests-on-the-pc)).

## Discussions

Use [Discussions](https://github.com/seedds/nxnative/discussions) for questions, ideas and reports on how games run on your Mac or iPhone. File bug reports in [Issues](https://github.com/seedds/nxnative/issues) and include the log. Follow frame rate, resolution and which Macs hold 60 fps in [Ocarina of Time: state on main](https://github.com/seedds/nxnative/discussions/1789).

## For developers

[How it works](docs/development.md) · [Game notes](dm/games/README.md#games) · [Bring-up and debugging](docs/agents/README.md) · [Front ends](docs/frontends.md) · [Contributing](CONTRIBUTING.md)

## Acknowledgements

This project is built on other people's work ([licenses](dm/third_party/README.md)):

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this native-execution approach, for Diablo II: Resurrected. nxnative started as a copy of its generic parts: the NSO rewriter, the host that stands in for Horizon, the NVN-on-Metal layer and the iOS app shell. Fixes go both ways.
- **Ryujinx** (MIT): its shader translator (`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` 1.3.2, in `dm/third_party/ryujinx` with a few fixes listed in [its README](dm/third_party/ryujinx/README.md)) turns the games' shaders into SPIR-V. Its emulation of the kernel and system services is also a reference when the host's behaviour is in doubt.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: its kernel, mesosphère, is the reference for Horizon's synchronization semantics (address arbiter, condition variables).
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reads NSPs and layers updates over the base game in `dm/tools/nx_extract`.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: SPIR-V to Metal Shading Language.
- **[glslang](https://github.com/KhronosGroup/glslang)** compiles the GLSL of OpenGL games to SPIR-V for it.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT): the GPU widening of 8-bit indices and in-pass clears in `dm/gfx/nvn/nvn_mtl.m`, and the visibility-counter lowering in the shader translator.

## License

MIT ([LICENSE](LICENSE)). Third-party code keeps its own license ([dm/third_party](dm/third_party/README.md)).
