# Troubleshooting

## ffmpeg / ffprobe not found

The packer needs both `ffmpeg` and `ffprobe` (`.exe` on Windows). Put them on your `PATH`, or point
the GUI's ffmpeg field at the folder that contains them. A full ffmpeg build includes both. On macOS
the app also looks in Homebrew's and MacPorts' folders, which an app opened from Finder does not
otherwise see.

If you'd rather not install it yourself on Windows, the **Download ffmpeg automatically** button (first-run setup
or **Settings...**) fetches a pinned static LGPL build from
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) into `.\ffmpeg` beside the app when that
folder is writable, otherwise `%LOCALAPPDATA%\ReSkateMusicPacker\ffmpeg`.

## The automatic ffmpeg download fails

- **Offline / blocked / a corporate proxy**: the download uses the Windows system proxy (WinHTTP) but
  needs to reach `github.com` and its asset host. Point the tool at a local ffmpeg folder instead
  (**Locate ffmpeg...**), or set `FFMPEG_URL` (and `FFMPEG_SHA256`) to a mirror.
- **HTTP 404**: the pinned release was pruned. Set `FFMPEG_URL` to a current asset and `FFMPEG_SHA256`
  to its SHA-256, or use **Locate ffmpeg...**. Release builds also re-check the pinned link before
  packaging, so this is normally caught before a release ships.
- **"did not match its expected checksum"**: the download was corrupted or the pinned asset changed.
  Retry; if it persists, update `FFMPEG_URL`/`FFMPEG_SHA256`.
- **SmartScreen / antivirus** may flag a freshly downloaded `ffmpeg.exe` because it is new and
  unsigned. That is expected for a first-run download; allow it, or install ffmpeg yourself.
- The download is about **163 MB** and is only started when you click, never on startup.

## Song clash warning

Every song id is the string `Artist - Title`. If a song's id already exists in another installed mod
or in the game's soundtrack, the mod can't add it (and a duplicate id can break the game's music
catalog). Rename the artist/title (or fix the file's tags) so the id is unique, then rebuild.

## Cover art doesn't show

- The runtime must support mod artwork. On an older ReSkate runtime the covers are ignored (music still
  plays).
- Check `reskate-music.json` references the file by a path relative to the mod folder, `.png`, with no
  `..` or `\`.
- The runtime only serves artwork for songs it can resolve by their `Artist - Title` id, so make sure
  the id in `song_artwork` matches the song exactly.

## Mod tracks sound quiet (or loud) next to the game's music

The packer normalises to **-15 LUFS** (true peak -1.5 dBTP) with two-pass `loudnorm`, the level the
game's own tracks sit at. If you disabled normalisation, or if you're comparing against a differently
mastered source, this will differ. Use `--no-normalize` to pack the source level unchanged.

## The build is slow, or it makes the machine unresponsive

Encoding is the slow part, and it runs several songs at once by default. Songs are cached by source
hash, so a rebuild only re-encodes what changed; deleting the cache folder forces everything again.

To trade speed for a responsive machine, lower the count in **Settings... → Parallel encoding**, with
`--jobs <n>` per build, or `RESKATE_JOBS` for scripts. The GUI shows the number it will actually use.
Raising it does not change the mod, only how fast it arrives. Note that each ffmpeg process asks for
several threads of its own, so more songs than logical cores usually makes things slower rather than
faster.

## The build fails with "cannot open ReSkateMusicPacker.exe"

A previous copy of the packer (or the GUI) is still running and holding the file. Close it and rebuild.

## Thunderstore export

The `.zip` is written beside the mod (or to the output you gave). Give it an author and version with
`--author` / `--version` (or the export dialog). It contains the full mod plus `manifest.json`, a
generated `README.md` tracklist and a 256x256 `icon.png`.

## The game doesn't see the mod

- Make sure the mod folder is in your ReSkate `Mods` folder and enabled in the launcher.
- ReSkate merges mods at launch; restart the game (or re-run the launcher) after adding a mod.
- The `.reskate-studio-patch` records the game build the mod targets. A mod built for a different game
  build may be left out by the merger; rebuild against the game folder you're playing.

## Where the files live

| | Windows | macOS | Linux |
|---|---|---|---|
| Settings (game folder, ffmpeg, parallel encoding) | `%APPDATA%\ReSkateMusicPacker\settings.json` | `~/Library/Application Support/ReSkateMusicPacker/settings.json` | `$XDG_CONFIG_HOME` or `~/.config/ReSkateMusicPacker/settings.json` |
| Encode cache (delete to force a full re-encode) | `%LOCALAPPDATA%\ReSkateMusicPacker\cache\` | `~/Library/Caches/ReSkateMusicPacker/cache/` | `$XDG_CACHE_HOME` or `~/.cache/ReSkateMusicPacker/cache/` |
| Downloaded ffmpeg | `.\ffmpeg` beside the app (portable) or `%LOCALAPPDATA%\ReSkateMusicPacker\ffmpeg` | - | - |

## macOS and Linux notes

- **"No file dialog was found" (Linux)**: install `zenity` or `kdialog`, or drag files and folders
  onto the window.
- **Clash warnings miss the game soundtrack**: the soundtrack list comes from ReSkate's content cache.
  Off Windows it is looked for inside the Wine/Proton prefix (`drive_c/users/*/AppData/Local/ReSkate/cache`)
  of the game folder you chose; a game folder outside a prefix has none to check.
- **Mods are a little larger than on Windows**: there is no Oodle encoder outside Windows, so the
  mod's archive is stored uncompressed, which the game reads like its own uncompressed blocks. The
  game's own data is still read through the open-source ooz decoder.
- **Lowercase folders on Linux**: the game spells its paths the Windows way (`Data/Win32/...`); on a
  case-sensitive file system the packer matches them regardless of case.
