# Getting started

## What you need

- **Windows 10/11 (x64)**, **macOS 11+** or a **64-bit Linux desktop**. On macOS and Linux, skate.
  runs under Proton, Wine or CrossOver; the packer only needs to read its install folder.
- **ffmpeg and ffprobe** on your `PATH`, or in a folder you point the tool at. They handle decoding,
  Opus encoding, album-art extraction, and loudness measurement. Get them from
  [ffmpeg.org](https://ffmpeg.org/download.html) (a full build includes both binaries), or on macOS
  `brew install ffmpeg`, on Linux your package manager (`sudo apt install ffmpeg`, ...).
- A **ReSkate runtime** that reads `reskate-music.json` (mod playlists). Artwork needs the runtime's
  mod-artwork support as well.
- Your **game folder**: the ReSkate install containing `Skate.exe` (e.g. `F:\Games\ReSkate-1.0.0`).
  The packer reads the game's audio templates from it. Under Steam/Proton that is
  `~/.local/share/Steam/steamapps/common/<game>`; in a Wine or CrossOver bottle, the folder inside
  its `drive_c`.

## Building the packer

If you don't have a release build, see [Building from source](building.md).

## The GUI

Run `ReSkateMusicPacker.exe` (Windows), `ReSkateMusicPacker.app` (macOS) or `ReSkateMusicPacker`
(Linux) with no arguments, or double-click it.

1. **Game folder** - point it at the install with `Skate.exe`. The tool checks for the file.
2. **ffmpeg** - if ffmpeg/ffprobe aren't on `PATH`, point this at the folder containing them, or click
   **Download ffmpeg automatically** to fetch a pinned static LGPL build (from
   [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds)) into `.\ffmpeg` beside the app when
   that folder is writable, otherwise `%LOCALAPPDATA%\ReSkateMusicPacker\ffmpeg`. The download only
   starts from the click, and the archive is checked against a pinned SHA-256 before anything is
   extracted. The automatic download is Windows-only; on macOS and Linux install ffmpeg with your
   package manager and press **Check again**. Both settings are remembered in the settings file (see
   [where the files live](troubleshooting.md#where-the-files-live)).
   Songs are encoded several at a time by default. **Settings... → Parallel encoding** changes how many,
   and shows the count it will actually use (see [encoding in parallel](cli.md#encoding-in-parallel)).
3. **Add songs** - drag files or a folder onto the window, or use the picker. Each song shows its
   artist/title (from tags, else the file name) and an artwork thumbnail.
4. **Name and playlist** - give the mod a name and the playlist it should appear under in the in-game
   music menu. Songs can also be assigned to their own playlists for a multi-playlist mod.
5. **Artwork (optional)** - open **Artwork...** to override a song's cover, or generate a text cover
   for a playlist. Covers are converted to 512x512 PNGs and saved with the mod; **Use automatic**
   falls back to embedded album art.
6. **Build** - the progress bar runs encoding, then building the mod. The mod folder is written next
   to your songs (or to the output folder you chose), and its project file is saved inside so you can
   **reopen and rebuild** it later.
7. **Export Thunderstore... (optional)** - produces an upload-ready `.zip` (see the
   [CLI reference](cli.md#thunderstore) for what it contains).

## Getting your mod into the game

Copy the built mod folder into your ReSkate `Mods` folder and enable it in the launcher. ReSkate
merges mod content at launch; the songs appear in the music menu under your playlist.

Keep the mod folder (and its `reskate-music-project.json`) if you want to edit and rebuild it later -
the project records where your source audio and images are.

## Loudness

Songs are normalised to **-15 LUFS** (true peak -1.5 dBTP) using two-pass ffmpeg `loudnorm`, matching
the level of the game's own tracks so mod songs don't sound quiet beside them. Pass `--no-normalize`
(or untick it in the GUI) to disable this.
