"""Every parameter must actually change the picture.

A GLSL uniform name that does not match the C++ is silently ignored:
glGetUniformLocation returns -1, glUniform on -1 is a documented no-op, and
nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage is switched on, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

--------------------------------------------------------------- the three traps

**Nine of these controls are the audio side, and a harness has no audio.**
Nothing fills the FFT buffer unless something puts it there, so every one of
them would read dead. `--tone` is what makes this test possible at all: it
pushes a synthetic click train in through the same call the host uses and drives
the clock from a frame counter. Every render below carries it.

**A settled envelope hides four more.** Once a steady tone has settled, the
picture no longer depends on Attack, Release, Sensitivity or Hold. The tone is
therefore a click train stopped mid-decay -- see `injectSpectrum` in mbtest.

**Several controls only exist in one mode.** Siting is invisible unless the
reconstruction is Smooth; Hold only exists in Step. That is what CONTEXT is for.

And one that is a property of the effect rather than of the test: **Link Chroma
does nothing unless H and V differ**, because linking sets V from H. The
baseline therefore gives the two axes different values, and unlinked lattices,
so that all four lattice controls genuinely reach the picture at once.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

MBTEST = "./build/mbtest"
SIZE = "480x270"
SCRATCH = tempfile.mkdtemp(prefix="macroblocksweep")

# A baseline with every stage active.
#
# The two axes deliberately differ and the links are off: that is the one
# configuration in which Chroma V, Luma V and both Link controls all reach the
# picture at once.
#
# Chroma Audio sits a little above centre rather than at an extreme, so that
# sweeping the lattice controls underneath it still moves the block size instead
# of running into the clamp at either end.
BASE = {
    "Chroma H": 0.42,
    "Chroma V": 0.28,
    "Link Chroma": 0,
    "Luma H": 0.30,
    "Luma V": 0.20,
    "Link Luma": 0,
    "Matrix": 0,
    "Average In": 0,
    "Sampling": 0,
    "Reconstruction": 0,
    "Siting": 0,
    "Block Steps": 1,
    "Audio Mode": 1,          # Follow
    "Chroma Audio": 0.62,
    "Chroma Band": 1,
    "Luma Audio": 0.58,
    "Luma Band": 0,
    "Attack": 0.25,
    "Release": 0.40,
    "Sensitivity": 0.50,
    "Hold": 0.40,
    "Show Grid": 0,
    "Mix": 1.0,
}

# Parameters that only exist in one mode, and the baseline change that switches
# that mode on.
CONTEXT = {
    # A block is a constant either way under Blocky; Siting decides where a
    # value lives only when something interpolates between them.
    "Siting": {"Reconstruction": 1},
    # Hold is the decay of a latched value, and nothing latches except in Step.
    "Hold": {"Audio Mode": 2},
    # Sensitivity is the onset threshold in Step and the level threshold in
    # Gate, and only the second of those is measurable HERE.
    #
    # In Step the latched value is a running maximum, so as long as the loudest
    # hits fire at every threshold -- which they do -- changing the threshold
    # only decides whether the quiet ones also fire, and the maximum is the same
    # either way. The control is alive; a picture at one instant cannot see it.
    # `mbtest --audio` measures that job directly, by counting onsets.
    "Sensitivity": {"Audio Mode": 3},
}

# Endpoints to sweep between, where 0 and 1 are the wrong pair.
ENDS = {
    "Matrix": (0, 3),
    "Average In": (0, 1),
    "Sampling": (0, 1),
    "Reconstruction": (0, 1),
    "Siting": (0, 1),
    "Block Steps": (0, 2),
    "Audio Mode": (0, 3),
    "Chroma Band": (0, 3),
    "Luma Band": (0, 3),
    "Preset": (1, 7),
    # At 1.0 the whole canvas is one block and the audio amount on top of it has
    # nowhere to go; 0.85 is coarse and still modulating.
    "Chroma H": (0.10, 0.85),
    "Chroma V": (0.10, 0.85),
    "Luma H": (0.10, 0.75),
    "Luma V": (0.10, 0.75),
    # Signed controls: 0.5 is off, so sweep either side of it rather than from
    # one extreme to the other, which would land on two mirror images.
    "Chroma Audio": (0.5, 0.95),
    "Luma Audio": (0.5, 0.95),
}

# Not controls: the About block is a text line and four buttons that open a
# browser, and the FFT buffer is the host's to fill.
SKIP_TYPES = {"text", "event", "buffer"}


def render(path, overrides):
    args = [MBTEST, "--out", path, "--size", SIZE, "--tone"]
    merged = dict(BASE)
    merged.update(overrides)
    for key, value in merged.items():
        args += ["--set", f"{key}={value}"]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        print("render failed:", result.stdout, result.stderr)
        sys.exit(1)
    with open(path, "rb") as handle:
        return handle.read()


def pixels(png):
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = struct.unpack(">I", png[i:i + 4])[0]
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", data[:8])
        if kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(height))


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    changed = 0
    total = 0
    count = len(pa) // 4
    for i in range(0, len(pa), 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / count * 100.0, total / count


def parameters():
    listing = subprocess.run([MBTEST, "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        print("could not list parameters:", listing.stderr)
        sys.exit(1)

    out = []
    for line in listing.stdout.strip().splitlines()[1:]:
        fields = line.split()
        if len(fields) < 4:
            continue
        kind = fields[-2]
        name = " ".join(fields[1:-2])
        if kind in SKIP_TYPES:
            continue
        out.append(name)
    return out


def main():
    if not os.path.exists(MBTEST):
        print(f"{MBTEST} not found -- build first")
        return 1

    names = parameters()
    print(f"{'parameter':<18} {'pixels changed':>15} {'mean delta':>11}   verdict")

    dead = []
    for name in names:
        low, high = ENDS.get(name, (0.0, 1.0))
        context = CONTEXT.get(name, {})

        a = render(os.path.join(SCRATCH, "a.png"), {**context, name: low})
        b = render(os.path.join(SCRATCH, "b.png"), {**context, name: high})

        percent, mean = difference(a, b)
        # A tenth of a per cent of the frame is a real change; anything below is
        # rounding between two renders of the same picture.
        alive = percent > 0.1
        if not alive:
            dead.append(name)

        note = "" if not context else "  (" + ", ".join(f"{k}={v}" for k, v in context.items()) + ")"
        print(f"{name:<18} {percent:>14.2f}% {mean:>11.3f}   {'ok' if alive else 'DEAD'}{note}")

    print()
    if dead:
        print("DEAD CONTROLS:", ", ".join(dead))
        print("Check the uniform name matches the GLSL, and that nothing in BASE masks it.")
        return 1

    print(f"all {len(names)} controls reach the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
