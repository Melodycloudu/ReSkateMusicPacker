# ReSkate Music Packer — Parallel Fork

[![Encoding: ~2.3× faster](https://img.shields.io/badge/encoding-~2.3%C3%97%20faster-brightgreen)](#benchmark)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Fork of DeckardDetribine/ReSkateMusicPacker](https://img.shields.io/badge/fork%20of-DeckardDetribine%2FReSkateMusicPacker-lightgrey)](https://github.com/DeckardDetribine/ReSkateMusicPacker)

> **This is a fork of [DeckardDetribine/ReSkateMusicPacker](https://github.com/DeckardDetribine/ReSkateMusicPacker).**
> It produces the same add-only music mods for **skate.** on [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate),
> but it spends less time doing it: **songs are encoded several at a time instead of one after another**.
> In measured runs on a 6-core/12-thread CPU, the encode stage of an 8-song build fell from
> **100.3 s to 43.4 s at the default 6 jobs (2.31× faster)**; across runs the gain was **1.95–2.50×**.
> The mod itself is unchanged — same format, same song order, same in-game result.
> [See the benchmark](#benchmark) and [the full list of changes](#what-changed-from-the-original).

A standalone tool for Windows, macOS and Linux (GUI **and** CLI) that turns your audio files into add-only music mods for
**skate.** running on [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate). It adds new songs and
custom playlists to the game's music without replacing any shipped files, so it survives game updates
and stacks with other mods.

![The ReSkate Music Packer window](docs/images/screenshot.png)

- **Documentation:** [`docs/`](docs/index.md)
- **Upstream project:** [DeckardDetribine/ReSkateMusicPacker](https://github.com/DeckardDetribine/ReSkateMusicPacker)
- **License:** [GPL-3.0](LICENSE) — includes engine code ported from [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) (see [NOTICE.md](NOTICE.md))

## Why this fork is faster

The original packer encodes songs one at a time. Encoding is the slowest part of a build: ffmpeg has
to decode the source, measure loudness (when normalization is on), and encode Opus. One song at a
time leaves most of a modern CPU idle.

This fork turns that loop into a worker pool:

- **Several songs encode at once.** Each worker runs its own ffmpeg process; the packer keeps one
  job per two logical CPUs by default (6 jobs on a 6-core/12-thread CPU), capped at 64.
- **The result is still deterministic.** Songs are collected by their original index and assembled
  in the order you gave them, so parallelism changes only the wall clock, not the mod. The test
  suite packs the same songs at 1, 2, 4 and 8 jobs and checks that the archive layout and tracklist
  are identical.
- **The loudness measurement is cached separately.** Rebuilding at a different bitrate re-encodes
  the audio without decoding every song a second time just to measure it.
- **The archive is sized once.** Chunk sizes are known before assembly, so the output buffer does
  not repeatedly reallocate and copy the whole library as it grows.
- **The expensive work is bounded.** `RESKATE_JOBS=4096` cannot fork-bomb the machine; values are
  capped at 64, and an invalid value falls back to Auto.

### Benchmark

Measured on **AMD Ryzen 5 5600 (6 cores / 12 threads)**, Windows, ffmpeg 2026-09-14, packing
**8 × 3-minute generated test tones**, Opus **192 kbps**, loudness normalization on, with the source
files already in the page cache. The benchmark runs the exact ffmpeg argument lists from
`src/packer.cpp` (loudnorm measurement, then the Opus encode) and compares the original
one-at-a-time loop with this fork:

| Encoding | Time | Speed-up |
|---|---:|---:|
| Original: 1 job at a time | 100.3 s | 1.00× |
| This fork: 6 jobs (Auto on this CPU) | 43.4 s | **2.31×** |
| This fork: 12 jobs (all logical CPUs) | 46.5 s | **2.16×** |

All job counts produced the same 34,370,873-byte output. Timings vary by 10–20% between runs; an
earlier sweep on the same machine measured 2.50× at 6 jobs and 2.78× at 8, but a repeat run put 12
behind 6. The best count is machine-specific, so Auto (half the logical CPUs) is a safe default and
it is worth trying a step up or down for your own library.

The benchmark isolates the **encode stage**, which is where nearly all of a large build's time goes.
A full mod build also spends time on asset/TOC assembly and archive writing, so the end-to-end
speed-up on a small library is smaller; on a large library it approaches the numbers above. The
exact gain depends on the CPU, the number of songs, the source format, bitrate, normalization, and
disk speed.

The benchmark is reproducible: `python scripts/benchmark-encode.py` generates the same test audio
and prints the table for the job counts you pass (`--songs`, `--seconds` and `--jobs` are
configurable). It needs Python 3.10+ and ffmpeg with libopus on `PATH`.

You can change the job count to suit your machine:

| Where | Example | Notes |
|---|---|---|
| GUI | **Settings... → Parallel encoding** | Saved between runs. Shows the count it will actually use. |
| CLI | `--jobs 4` | Per build, 1–64. |
| Environment | `RESKATE_JOBS=4` | For scripts and CI. Ignored by an explicit setting. |

Lower it if a build makes the machine unresponsive; raise it if you have cores to spare. More jobs
than logical cores usually makes things slower, not faster, because each ffmpeg process already
uses several threads of its own. See [encoding in parallel](docs/cli.md#encoding-in-parallel).

## What changed from the original

Everything below is on top of upstream `main` (`863dfe7`, after v1.1.2). Nothing in the mod format,
game requirements, or UI workflow was changed.

### 1. Parallel song encoding (`src/packer.cpp`, `src/packer.h`)

- `pack()` now builds an `EncodedSong` per song — chunks, wave document, song document, and
  sound-bank resource — in a worker pool, then assembles the playlist, chunks, assets and resources
  in original song order.
- Default job count is half the logical CPUs; `PackOptions::jobs`, `--jobs`, and `RESKATE_JOBS` can
  override it. All paths are capped at 64 and floored at 1.
- A failing song stops new songs from starting; the first failure is rethrown in song order, so the
  error you see does not depend on which worker finished first.
- Cancelling stops new songs from starting and lets the ones already encoding finish, then unwinds
  without leaving partial output.
- `src/main.cpp` shows the effective count in Settings, persists it in `settings.json`, adds
  `--jobs` to the CLI, and validates it (`--jobs eight` is a clear usage error, not a crash).
- Progress callbacks are serialized behind a mutex, so a caller-provided `ProgressFn` is never
  called re-entrantly; the GUI progress bar only moves forward even though songs finish out of
  order.

### 2. Loudness measurement cache (`src/packer.cpp`)

- The loudnorm measurement is stored separately from the encoded `.ogg`
  (`<source-hash>_measure_norm15.json`), so switching bitrate re-encodes without re-decoding every
  song just to measure it.
- Cached measurements are validated on read (prefix, required fields, and a `linear=true` tail), so
  a stale or truncated entry is measured again instead of trusted.

### 3. Cache and scratch-file safety (`src/packer.cpp`)

- Cache entries are written to a temporary file and **renamed** into place, so a rebuild can never
  read a half-written cache entry and a lost race simply drops the duplicate.
- Scratch filenames now include the song index. Two copies of the same audio file added under
  different names no longer hand `ffmpeg -y` the same output path and overwrite each other.
- The scratch directory is created with an error-code overload and checked afterwards, so several
  workers creating it at once is not a failure.
- Cover-preview scratch images are unique per call instead of sharing one fixed filename.
- The chunk/archive buffer is pre-sized after all songs are encoded, and per-song buffers are
  released with `swap()` once copied, keeping peak memory close to the original's.

### 4. Thunderstore export fixes (`src/packer.cpp`)

- Each export uses a fresh staging folder and a scope guard removes it even when packaging throws.
- Two exports (or an export during a build) can no longer tread on each other.
- Exporting into a folder inside the mod skips the previous `.zip`, so it is not swept into the
  next package.

### 5. Tests and CI (`test/packer_tests.cpp`, `.github/workflows/ci.yml`)

- New tests cover job-count arithmetic and validation, determinism across 1/2/4/8/4096/invalid job
  counts, song ordering with different-length tracks, duplicate source files under two names, and
  cancelling with a song in flight.
- A ThreadSanitizer CI job builds and runs the non-game tests, because a thread pool is exactly the
  kind of code where a race hides until it matters.
- `platform::set_environment()` was added for Windows and POSIX so tests can exercise the
  `RESKATE_JOBS` path without touching the parent environment.
- `scripts/benchmark-encode.py` reproduces the benchmark above with generated test tones.

### 6. Documentation

- `README.md` (this file), `CHANGELOG.md`, `docs/cli.md`, `docs/getting-started.md`, and
  `docs/troubleshooting.md` document the job setting, its precedence, and when to lower it.

## Features

All upstream features are included:

- **Hybrid GUI & CLI** - double-click for a Dear ImGui window (DirectX 12 on Windows, SDL2 on macOS
  and Linux), or script it from the command line for batch packing.
- **Add-only mods** - songs are added, never replacing the game's own tracks.
- **Custom playlists** - group songs into named playlists that appear in the in-game music menu.
- **Track & playlist artwork** - embedded album art is extracted automatically; pick your own image or
  generate a text cover, with a live preview. Covers are 512x512 PNGs saved with the mod.
- **Match the game's loudness** - EBU R128 two-pass `loudnorm` to **-15 LUFS** (true peak -1.5 dBTP),
  the level of the game's own tracks, so mod songs don't sound quiet next to them.
- **Encode cache** - transcoded Opus is cached by source hash, so rebuilding only re-encodes what
  changed.
- **Song clash warning** - checks artist/title against installed mods and the game's soundtrack so you
  don't collide with an existing song id.
- **Thunderstore export** - one click to a Thunderstore-ready `.zip` (`manifest.json`, generated
  README/tracklist, 256x256 icon).
- **One-click ffmpeg install (Windows)** - if ffmpeg/ffprobe are missing, download a pinned static LGPL build
  (BtbN/FFmpeg-Builds) into `.\ffmpeg` beside the app or `%LOCALAPPDATA%\ReSkateMusicPacker\ffmpeg`,
  verified against a SHA-256 before it is used.
- **High-DPI aware** - per-monitor v2 DPI scaling on Windows, Retina on macOS, the desktop scale on Linux.
- **Cross-platform** - on macOS and Linux the game's Oodle-compressed data is read with the
  open-source [ooz](https://github.com/zao/ooz) decoder, so no Windows DLL is needed; there the
  mod's archive is written uncompressed (a little larger, read the same way by the game).

## Requirements

- Windows 10/11 (x64), macOS 11+ (Apple Silicon or Intel), or a 64-bit Linux desktop (x86-64 or
  ARM64). On macOS and Linux the game itself runs under Proton/Wine/CrossOver; point the packer at
  that install folder.
- **[ffmpeg](https://ffmpeg.org/) and ffprobe** on your `PATH`, or in a folder you point the GUI at. They do the decoding,
  Opus encoding, album-art extraction, and loudness measurement. If they're missing, the GUI's
  **Download ffmpeg automatically** button fetches a pinned static LGPL build for you (only when you
  click it). ffmpeg is only needed to *build* mods, not to play them in game.
- **No installer and no Visual C++ redistributable** on Windows: the executable is self-contained
  (statically linked) and uses only Windows system libraries. On macOS and Linux it needs SDL2
  (`brew install sdl2 ffmpeg`, `sudo apt install libsdl2-2.0-0 ffmpeg`).
- A **ReSkate runtime that reads `reskate-music.json`** (mod playlists). Cover art additionally needs
  the runtime's mod-artwork support.

## Quick start (GUI)

1. Run `ReSkateMusicPacker.exe` with no arguments (or double-click it).
2. Set your **game folder** (the one containing `Skate.exe`) and, if ffmpeg isn't on `PATH`, its
   folder.
3. Drop songs or a folder onto the window (or use **Add songs...**), then set the mod name and
   playlist.
4. Optionally open **Artwork...** to override track art or generate a playlist cover.
5. **Build**. The mod folder is written next to your songs (or to the output folder you choose). Copy
   it into `F:\...\Mods\` (your ReSkate `Mods` folder) and enable it in the launcher.

Songs encode several at a time by default. **Settings... → Parallel encoding** changes how many and
shows the count it will actually use.

## Quick start (CLI)

```powershell
ReSkateMusicPacker.exe "<game folder>" "<song folder>" [output folder] `
    --name "My Mix" --playlist "My Mix" `
    --playlist-artwork "My Mix" "cover.png" `
    --track-artwork "Artist - Title" "track.png"
```

Add `--jobs 4` to choose how many songs encode at once. See the [CLI reference](docs/cli.md) for
every option.

## Supported audio

Anything ffmpeg can decode. The GUI file picker and the drop handler list
`.mp3 .flac .ogg .opus .wav .m4a .aac .wma .aiff .aif .webm .mka .mp4`; the CLI scans a folder for
non-image files and lets ffmpeg decide. Audio is encoded to stereo 48 kHz Opus (default 192 kbps,
64-320).

## What it produces

A ReSkate mod folder containing the add-only content (a TOC + `.cas` with the new `_mg`/`_nwa` assets
and audio chunks), a `reskate-music.json` describing your playlists and covers, the artwork PNGs, and
the project/build files so the mod can be reopened and rebuilt. See
[The mod format](docs/mod-format.md).

## Documentation

| Guide | Contents |
|---|---|
| [Getting started](docs/getting-started.md) | Install, GUI walkthrough, getting it into the game |
| [CLI reference](docs/cli.md) | Every flag, with examples and batch use |
| [The mod format](docs/mod-format.md) | What's in a mod, `reskate-music.json`, artwork, ReSkate requirements |
| [Building from source](docs/building.md) | Prerequisites and CMake |
| [Troubleshooting](docs/troubleshooting.md) | ffmpeg, clash warnings, missing covers, loudness, slow builds, Thunderstore |

## Building

Windows: Visual Studio 2022 (v143) and CMake 3.20+. macOS/Linux: a C++20 compiler, CMake 3.20+
and SDL2. See [docs/building.md](docs/building.md).

## Credits and license

This fork is maintained by [Melodycloudu](https://github.com/Melodycloudu). The original project and
the vast majority of this code are by **DeckardDetribine and the ReSkateMusicPacker contributors**;
see the [upstream repository](https://github.com/DeckardDetribine/ReSkateMusicPacker).

Copyright © 2026 DeckardDetribine and the ReSkateMusicPacker contributors. ReSkateMusicPacker is
free software, distributed under the [GNU General Public License, version 3](LICENSE) (GPL-3.0);
if you share a modified version, share its source code under the same license.

It includes engine code ported from the [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate)
project (Copyright © 2026 the ReSkate contributors, also GPL-3.0), which is why this project is
GPL-3.0 rather than a permissive license. See [NOTICE.md](NOTICE.md) for provenance and for the
libraries under `External/`, which keep their own licenses.
