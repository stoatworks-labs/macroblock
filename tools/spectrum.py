"""A per-frame spectrum file, out of an audio track, for `mbtest --pipe`.

    python3 tools/spectrum.py track.wav --fps 60 --frames 1800 -o spectrum.txt

One line per video frame, 64 numbers each, in the same shape Resolume fills its
`FF_USAGE_FFT` buffer parameter with. `mbtest --pipe --spectrum FILE` pushes each
line in through `SetParamElementValue` — the identical call the host makes — so
the plugin's own `Audio.cpp` is what moves the lattice.

That is the whole point of this script. The easy way to make an audio-reactive
plugin *look* audio-reactive in a rendered video is to key the block size in the
cue sheet so it lands on the beats, and it proves nothing at all, because the
analyser is never involved. This way the footage is a genuine demonstration:
real music, the real onset detector, the real envelopes.

------------------------------------------------------------ the assumption

**Nobody here knows how Resolume maps its 64 bins to frequency**, and the FFGL
header does not say. `Audio.cpp` splits the band ranges by *bin index* — 0..7,
8..27, 28..63 — so whatever the host's mapping is, "Low" means those indices
there. This script has to pick a mapping to produce the numbers at all.

The default is what a plain 1024-point FFT at 44.1 kHz gives if you take the
first 64 bins: linear, about 43 Hz per bin, topping out near 2.7 kHz. Under it
the three bands come out at roughly 0–345 Hz, 345–1200 Hz and 1.2–2.8 kHz, which
are defensible crossover points and match what `Audio.cpp`'s comment claims the
split is for.

**If Resolume's mapping turns out to be different, the bands will pick different
content in the host than they do here**, and the fix is to change the ranges in
`Audio.cpp` rather than this file. That is a question only a session with Arena
open can settle; see the bottom of AGENTS.md. `--fmax` is here so a later
measurement can be tried without editing anything.

Magnitudes are normalised so a well-mastered track peaks near 1. The host's
scaling is unknown too, but the analyser normalises every band against its own
recent peak, so a constant factor either way comes out in the wash — which is
the one part of this that is robust to being wrong.
"""
import argparse
import subprocess
import sys

import numpy as np

BINS = 64
RATE = 48000


def decode(path):
    """Mono float32 at 48 kHz, via ffmpeg."""
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path,
         "-f", "f32le", "-ac", "1", "-ar", str(RATE), "-"],
        capture_output=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode(errors="replace"))
        raise SystemExit(f"ffmpeg could not decode {path}")
    return np.frombuffer(result.stdout, dtype=np.float32)


def spectrum(samples, fps, frames, fmax, window):
    """`frames` rows of `BINS` magnitudes."""
    hop = RATE / fps
    hann = np.hanning(window).astype(np.float32)

    # Which FFT bins fall in each of our 64, under the mapping in the docstring.
    freqs = np.fft.rfftfreq(window, 1.0 / RATE)
    edges = np.linspace(0.0, fmax, BINS + 1)
    index = np.digitize(freqs, edges) - 1

    out = np.zeros((frames, BINS), dtype=np.float32)

    for frame in range(frames):
        # Centred on the frame, so a transient lands on the frame it belongs to
        # rather than one late. The plugin cannot do better than a frame either
        # way -- see the note at the top of Audio.h -- but there is no reason to
        # add a whole frame of lag here on top of it.
        centre = int(frame * hop)
        start = max(0, centre - window // 2)
        chunk = samples[start:start + window]
        if len(chunk) < window:
            chunk = np.pad(chunk, (0, window - len(chunk)))

        magnitude = np.abs(np.fft.rfft(chunk * hann))

        for b in range(BINS):
            selected = magnitude[index == b]
            if selected.size:
                out[frame, b] = selected.mean()

    # One scale factor for the whole file, from a high percentile rather than
    # the maximum: a single sample-rate click would otherwise set the level for
    # the entire piece and everything else would sit near zero.
    reference = np.percentile(out, 99.5)
    if reference > 0:
        out /= reference

    return np.clip(out, 0.0, 4.0)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("audio")
    parser.add_argument("-o", "--out", default="spectrum.txt")
    parser.add_argument("--fps", type=float, default=60.0)
    parser.add_argument("--frames", type=int, default=None,
                        help="default: as many as the audio lasts")
    parser.add_argument("--fmax", type=float, default=RATE / 1024 * 64,
                        help="top of the mapped range in Hz; see the module docstring")
    parser.add_argument("--window", type=int, default=2048,
                        help="FFT size in samples")
    args = parser.parse_args()

    samples = decode(args.audio)
    seconds = len(samples) / RATE
    frames = args.frames if args.frames else int(seconds * args.fps)

    sys.stderr.write(
        f"{args.audio}: {seconds:.1f}s, {frames} frames at {args.fps:g} fps, "
        f"{BINS} bins to {args.fmax:.0f} Hz\n"
    )

    rows = spectrum(samples, args.fps, frames, args.fmax, args.window)

    with open(args.out, "w") as handle:
        handle.write(f"# {BINS} bins per line, {frames} frames at {args.fps:g} fps\n")
        handle.write(f"# mapped linearly to {args.fmax:.0f} Hz -- an assumption, see tools/spectrum.py\n")
        for row in rows:
            handle.write(" ".join(f"{v:.5f}" for v in row) + "\n")

    sys.stderr.write(f"wrote {args.out}\n")


if __name__ == "__main__":
    main()
