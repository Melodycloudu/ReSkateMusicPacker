# CLI reference

```text
ReSkateMusicPacker.exe <game folder> <song folder> [output folder] [options]
```

- **game folder** - the ReSkate install containing `Skate.exe`.
- **song folder** - a folder of audio files. The CLI reads the files directly in that folder (it does
  not recurse), ignoring images.
- **output folder** - where the mod is written. Defaults to `<song folder>_mod` next to the song
  folder.

Run with no arguments (or `--gui`) for the graphical interface.

## Options

| Flag | Description | Default |
|---|---|---|
| `--name <mod name>` | Display name of the mod. | Folder name |
| `--playlist <playlist>` | Playlist name shown in the in-game music menu. | Folder name |
| `--bitrate <kbps>` | Opus bitrate, 64-320. | `192` |
| `--no-normalize` | Disable EBU R128 loudness normalisation (two-pass to -15 LUFS). | Normalised |
| `--jobs <n>` | Songs encoded at once, 1-64. Affects speed only, never the mod. | Half the logical CPUs |
| `--thunderstore` | Also export a Thunderstore-ready `.zip` (see below). | Off |
| `--author <name>` | Author namespace in the Thunderstore manifest. | `Author` |
| `--version <x.y.z>` | Thunderstore package version. | `1.0.0` |
| `--playlist-artwork <name> <image>` | Cover image for the named playlist. | None |
| `--track-artwork <id> <image>` | Cover for the exact `Artist - Title` song id; repeat for several. | None |
| `--generate-playlist-artwork <name>` | Generate a text cover from the playlist name. | Off |
| `--get-ffmpeg [folder]` | Download the pinned static ffmpeg build and print the install path, then exit. | `.` or `%LOCALAPPDATA%` |
| `--gui` | Force the graphical interface. | |
| `--help`, `-h` | Show usage. | |

`PNG`, `JPEG`, `WebP`, `BMP` (and other WIC-readable) images can be used for artwork. Covers are scaled
proportionally to a 512x512 PNG with dark padding, saved inside the mod.

## Examples

Basic pack:

```powershell
ReSkateMusicPacker.exe "F:\Games\ReSkate-1.0.0" "C:\Music\My Mix" --playlist "My Mix"
```

Full: playlist cover, a per-track cover, and a Thunderstore zip:

```powershell
ReSkateMusicPacker.exe "F:\Games\ReSkate-1.0.0" "C:\Music\My Mix" "C:\Mods\My Mix" `
    --name "My Mix" --playlist "My Mix" --bitrate 224 `
    --playlist-artwork "My Mix" "C:\Pictures\mix.png" `
    --track-artwork "MGMT - Kids" "C:\Pictures\kids.png" `
    --thunderstore --author MyName --version 1.0.0
```

Generated playlist cover instead of an image:

```powershell
ReSkateMusicPacker.exe "F:\Games\ReSkate-1.0.0" "C:\Music\My Mix" --generate-playlist-artwork "My Mix"
```

Fetch ffmpeg (no game or songs needed):

```powershell
ReSkateMusicPacker.exe --get-ffmpeg
ReSkateMusicPacker.exe --get-ffmpeg "C:\Tools\ffmpeg"
```

`--get-ffmpeg` (Windows only) downloads the same pinned static LGPL build (BtbN/FFmpeg-Builds) the GUI button uses,
verifies its SHA-256, extracts `ffmpeg.exe` + `ffprobe.exe` and prints the folder. With no folder it
installs beside the exe when writable, else under `%LOCALAPPDATA%\ReSkateMusicPacker\ffmpeg`.
`FFMPEG_URL` and `FFMPEG_SHA256` override the pinned source.

## Caching

Encoded Opus is cached by source-file hash and settings in
`%LOCALAPPDATA%\ReSkateMusicPacker\cache\` (see [where the files live](troubleshooting.md#where-the-files-live)
for macOS and Linux), so rebuilding only re-encodes what changed. Deleting that
folder forces a full re-encode.

The loudness measurement is cached separately from the audio, so rebuilding at a
different bitrate re-encodes without decoding every song again just to measure it.

## Encoding in parallel

Songs are encoded in parallel: several ffmpeg processes run at once, each of which
wants several threads of its own. By default the packer starts one job per two
logical CPUs, which keeps the machine busy without every process fighting for every
core. Songs still come out in the order you gave them, and the number of jobs never
changes the mod itself.

Choose it in three places, in order of precedence:

| Where | Example | Notes |
|---|---|---|
| `--jobs <n>` | `--jobs 4` | Per build. 1-64. |
| GUI: Settings → Parallel encoding | | Saved between runs, so the GUI remembers. |
| `RESKATE_JOBS` | `$env:RESKATE_JOBS = 2` | Environment, for scripts and CI. |

`RESKATE_JOBS` is only consulted when neither `--jobs` nor the saved GUI setting asks
for a specific number. It has to be a plain positive number: `0`, a negative value, or
anything with other characters in it is ignored and the default is used. On macOS and
Linux:

```bash
RESKATE_JOBS=2 ./ReSkateMusicPacker "/games/ReSkate" "~/Music/My Mix"
```

Lower it if the pack makes the machine unresponsive; raise it if you would rather
leave the other cores free. The GUI shows the number it will actually use, so there is
no need to work it out by hand.

Cancelling stops new songs from starting, but the ones already being encoded are
allowed to finish, so a cancel can take a moment.

## Thunderstore

`--thunderstore` writes `<Author>-<Name>-<Version>.zip` (or the `--author`/`--version` you gave) beside
the mod. It contains the whole mod plus a Thunderstore `manifest.json`, a generated `README.md`
tracklist, and a 256x256 `icon.png` (custom if you provide one, otherwise generated).
