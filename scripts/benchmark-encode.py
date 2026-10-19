#!/usr/bin/env python3
"""Benchmark the encode stage ReSkateMusicPacker runs: serial vs parallel.

Each song is measured with ffmpeg loudnorm (the same two-pass measurement the
packer does) and then encoded to Opus with the same argument lists as
src/packer.cpp. This isolates the part of a build that the fork parallelises; a
full mod build also spends time on asset/TOC assembly and archive writing.

Usage:
    python scripts/benchmark-encode.py
    python scripts/benchmark-encode.py --songs 12 --seconds 240 --jobs 1 4 8
    python scripts/benchmark-encode.py --keep

Requires Python 3.10+ and ffmpeg (with libopus) on PATH.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

FILTER = "loudnorm=I=-15.0:TP=-1.5:LRA=11"


def run(args: list[str]) -> str:
    result = subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(f"{args[0]} failed: {result.stderr.decode(errors='replace')[-2000:]}")
    return result.stderr.decode(errors="replace")


def make_audio(folder: Path, songs: int, seconds: int) -> list[Path]:
    folder.mkdir(parents=True, exist_ok=True)
    files = []
    for index in range(songs):
        path = folder / f"song{index + 1}.mp3"
        if not path.exists():
            run([
                "ffmpeg", "-y", "-v", "error", "-f", "lavfi",
                "-i", f"sine=frequency={220 + index * 55}:duration={seconds}",
                "-ac", "2", "-ar", "48000", "-b:a", "192k", str(path),
            ])
        files.append(path)
    return files


def measure(path: Path) -> str:
    text = run([
        "ffmpeg", "-hide_banner", "-nostats", "-i", str(path), "-vn", "-map_metadata", "-1",
        "-ac", "2", "-ar", "48000", "-af", f"{FILTER}:print_format=json", "-f", "null", "-",
    ])
    match = re.search(r"\{[^{}]*\"input_i\"[^{}]*\}", text, re.S)
    if not match:
        raise RuntimeError("no loudnorm JSON in ffmpeg output")
    data = json.loads(match.group(0))
    return (
        f"{FILTER}:measured_I={data['input_i']}:measured_LRA={data['input_lra']}"
        f":measured_TP={data['input_tp']}:measured_thresh={data['input_thresh']}"
        f":offset={data['target_offset']}:linear=true"
    )


def encode_one(source: Path, out: Path, bitrate: int) -> int:
    af = measure(source)
    run([
        "ffmpeg", "-y", "-v", "error", "-i", str(source), "-vn", "-map_metadata", "-1",
        "-ac", "2", "-ar", "48000", "-af", af, "-c:a", "libopus",
        "-b:a", f"{bitrate}k", "-vbr", "on", "-frame_duration", "20",
        "-application", "audio", "-f", "ogg", str(out),
    ])
    return out.stat().st_size


def timed(files: list[Path], out: Path, workers: int, bitrate: int) -> tuple[float, int]:
    out.mkdir(parents=True, exist_ok=True)
    for path in out.glob("*.ogg"):
        path.unlink()
    start = time.perf_counter()
    if workers == 1:
        for index, source in enumerate(files):
            encode_one(source, out / f"song{index + 1}.ogg", bitrate)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            list(pool.map(
                lambda pair: encode_one(pair[1], out / f"song{pair[0] + 1}.ogg", bitrate),
                enumerate(files),
            ))
    elapsed = time.perf_counter() - start
    total = sum(path.stat().st_size for path in out.glob("*.ogg"))
    return elapsed, total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--songs", type=int, default=8, help="number of songs to encode (default: 8)")
    parser.add_argument("--seconds", type=int, default=180, help="length of each generated song (default: 180)")
    parser.add_argument("--bitrate", type=int, default=192, help="Opus bitrate in kbps (default: 192)")
    parser.add_argument("--jobs", type=int, nargs="+", default=[1, max(1, (os.cpu_count() or 2) // 2)],
                        help="job counts to compare (default: 1 and Auto, half the logical CPUs)")
    parser.add_argument("--keep", action="store_true", help="keep the generated audio and output")
    args = parser.parse_args()

    if not shutil.which("ffmpeg"):
        print("error: ffmpeg is not on PATH", file=sys.stderr)
        return 1
    if args.songs < 1 or args.seconds < 1 or args.bitrate < 64:
        print("error: songs and seconds must be positive, bitrate at least 64", file=sys.stderr)
        return 1
    if any(jobs < 1 for jobs in args.jobs):
        print("error: job counts must be 1 or more", file=sys.stderr)
        return 1

    folder = Path(tempfile.mkdtemp(prefix="reskate-bench-"))
    try:
        audio = make_audio(folder / "audio", args.songs, args.seconds)
        for path in audio:
            path.read_bytes()  # warm the page cache so the comparison is CPU-bound
        print(f"{args.songs} songs x {args.seconds}s, Opus {args.bitrate}k, loudnorm on", flush=True)

        results = []
        for workers in args.jobs:
            elapsed, size = timed(audio, folder / "out", workers, args.bitrate)
            results.append((workers, elapsed, size))
            print(f"{workers:2d} job(s): {elapsed:7.2f} s  ({size} bytes)", flush=True)

        baseline = results[0]
        print()
        print("| Jobs | Time | Speed-up |")
        print("|---:|---:|---:|")
        for workers, elapsed, _ in results:
            print(f"| {workers} | {elapsed:.1f} s | {baseline[1] / elapsed:.2f}x |")
        if len({size for _, _, size in results}) != 1:
            print("warning: output sizes differ between job counts", file=sys.stderr)
            return 1
    finally:
        if args.keep:
            print(f"kept {folder}")
        else:
            shutil.rmtree(folder, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
