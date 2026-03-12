# iocod

`iocod` is an experimental standalone engine fork of
[ioquake3](https://github.com/ioquake/ioq3) aimed at reconstructing enough of
the original *Call of Duty* (2003) engine behavior to work with CoD1-style
assets, UI, scripts, and networking.

This is not stock ioquake3, and it is not an official Call of Duty source
release. If you want upstream Quake III engine work, use ioquake3 directly. If
you want to understand or extend this fork's Call of Duty compatibility work,
this repository is the right place.

## What This Repository Is
Call of Duty 1 compatibility and replacement effort

The default standalone build is branded as `iocod`, uses `main/` as its base
game directory, and produces:

- `iocod` for the client
- `iocod-ded` for the dedicated server
- `main/` game modules built from this source tree

## Current Status

This fork already contains substantial CoD-oriented work, but it is still an
in-progress engine port rather than a finished drop-in replacement.

- Multiplayer is the current focus.
- Single-player launch paths are intentionally not implemented in the current
  standalone UI.
- The standalone path is currently English-focused.
- Compatibility is incomplete; expect rough edges, missing features, and
  behavior that still differs from the original binaries.
- No proprietary game data is bundled here.

## Building

This fork keeps ioquake3's CMake-based build system. The commands below assume
the default standalone configuration.

On Linux or other Unix-like systems, a typical build is:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On Debian/Ubuntu, install at least:

```sh
sudo apt install cmake ninja-build libsdl2-dev
```

On Windows, use a generator that fits your toolchain, for example:

```sh
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

Build outputs are written under `build/<config>/`, for example:

- `build/Release/iocod`
- `build/Release/iocod-ded`
- `build/Release/main/`

The repository's CI currently builds this code on Linux, Windows, macOS,
Emscripten, FreeBSD, and OpenBSD.

## Game Data

You must supply your own retail *Call of Duty* data. This repository does not
include `main/pak*.pk3`, localized packs, or any other proprietary game
content.

The engine expects a CoD-style `main/` directory. The simplest approach is to
point `fs_basepath` at an existing Call of Duty installation:

```sh
./build/Release/iocod +set fs_basepath "/path/to/Call of Duty"
./build/Release/iocod-ded +set fs_basepath "/path/to/Call of Duty"
```

On Linux standalone builds, the engine also probes the default Steam/Proton
location at `~/.steam/steam/steamapps/common/Call of Duty`.

If you are targeting the web build, the expected `main/` asset layout is listed
in [`code/web/client-config.json`](code/web/client-config.json).

## Repository Layout

- [`code/`](code/) contains the engine, renderer, client, server, game code,
  and bundled third-party sources that are compiled as part of the project.
- [`cmake/`](cmake/) contains the build definitions, including the `iocod`
  project identity and standalone defaults.
- [`docs/`](docs/) contains retained ioquake3 documentation, renderer notes,
  and the preserved original id source release readme in
  [`docs/id-readme.txt`](docs/id-readme.txt).
- [`ref/`](ref/) contains reverse-engineering references, asset tools, and
  external material kept with their own readmes and licenses.

## Credits And Provenance

- id Software for the Quake III Arena GPL source release that made ioquake3,
  and therefore this fork, possible.
- The ioquake3 project and contributors for the cross-platform engine base this
  repository extends: <https://github.com/ioquake/ioq3>
- Infinity Ward and Activision for the original *Call of Duty* whose data
  formats, assets, scripting conventions, and game behavior this project
  targets for compatibility.

This repository is an independent fork. It does not include or relicense the
original Call of Duty game assets.

## License

The core engine code in this repository is derived from Quake III Arena and
ioquake3 and is distributed under the GNU GPL. See [`COPYING.txt`](COPYING.txt).

Bundled or referenced third-party components may carry additional license
terms. Those notices are kept alongside the relevant material, especially under
[`code/thirdparty/`](code/thirdparty/) and [`ref/`](ref/).

Original *Call of Duty* game assets, packs, and other proprietary content are
not included and remain the property of their respective rightsholders.
