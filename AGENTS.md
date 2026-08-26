# macroblock — orientation for another LLM (or a newcomer)

Chroma subsampling at any level, as an FFGL effect for Resolume and an OpenFX
effect for Resolve.

`CLAUDE.md` is the command reference. This file is the *why*.

---

## The one idea

**This is a sampling lattice, not a mosaic filter.**

Everything on the screen is the consequence of one decision: the picture is
split into a channel that carries detail and two that carry colour, the colour
channels are sampled on a coarser grid than the picture, and the picture is put
back together from what survived. That is what 4:2:0 is. This plugin makes the
grid arbitrary — from the two-pixel pair a codec uses up to the entire canvas —
and then does the same thing to luma on a second, independent grid, which no
sampling format has ever offered.

If you are ever tempted to draw a block, stop. A block is not drawn; it is what
you see when a colour is only sampled once per block. The distinction is not
pedantry, and two things follow from it that a mosaic filter does not get:

- **Luma detail survives completely.** Subsample chroma to a single value for
  the whole frame and every edge, every line pair, every bit of texture is still
  exactly where it was. A mosaic destroys detail; this does not touch it. The
  Fine detail clip on the demo page is the one-click demonstration.
- **The artefacts land in the right places.** A saturated red against blue gets a
  column of mixed blocks at the seam because the sampling grid straddles it, and
  that is the artefact real video has. Nothing draws it.

The rule is made arithmetic in one place, and it is load-bearing:
`sampling::blockRange` must **partition** the axis. Every source pixel belongs to
exactly one cell, none is counted twice and none is missed, for any real block
size including all the ones that do not divide the frame. Everything else is a
consequence of that being true.

---

## The shape of it

```
ProcessOpenGL:
  1 clock.Update( hostTime )        ms/seconds auto-detect; dt clamped
  2 analyser.Update( fft, dt )      64 bins -> per-band envelope, onset, gate
  3 controls::settings( ... )       0..1 -> a lattice, modulated by 2
  4 sampling::grid( ... )           block sizes and cell counts, per component
  5 reduce X   source -> (cells x height)   every pixel read ONCE
  6 reduce Y   that   -> (cells x cells)
  7 (5 and 6 again for luma, unless the two lattices are identical)
  8 composite  source + grids -> the host's framebuffer
```

Steps 5–7 are skipped whenever the corresponding lattice is inactive, which for
luma is the default.

### Why the reduction is separable

A box mean over a B×B block is separable, and here that is not a micro-
optimisation — it is the difference between the effect being usable and not.

Gathering the block in one pass costs B² taps per output texel. The output grid
shrinks as B grows, so the frame's **total** is W·H taps either way; but the
*worst fragment* does B² of them, and at a block covering a 4K frame that is
eight million taps in one invocation. Separated, the frame's total is still W·H
— every source pixel is read exactly once in reduce X — and no fragment does
more than B.

The consequence worth remembering: **this effect costs the same at every block
size.** A two-pixel chroma block and a whole-canvas one read the frame the same
number of times. There is no quality control to offer and no reason to want one.

### Directories

- `source/Sampling.{h,cpp}` — the colour matrices and the lattice. The one place
  either is defined.
- `source/Render.{h,cpp}` — the whole effect on the CPU. **This is the OpenFX
  renderer**, not a test double; see below.
- `source/shaders/` — the GPU. `Convert.cpp` mirrors Sampling.cpp in GLSL and
  says so in both directions.
- `source/Audio.{h,cpp}` — spectrum in, one number per band out. No GL, no FFGL.

---

## Traps

Ordered by how much time they will cost you.

### `blockRange` is the whole interface — read `Sampling.h` first

Two bugs lived in eight lines of it, and `mbtest --partition` found both:

**A cell's end must be `(cell + 1) * size`, never `cell * size + size`.** They
are the same number in arithmetic and not in float. This cell's end and the next
cell's start have to be the *same expression* or they disagree in the last bit —
at a block size of 10.1 that happens somewhere around cell 15 — and one pixel
lands in two blocks. The symptom is a single column that is subtly the wrong
colour on some block sizes and not others.

**`cells` is not `ceil(span / size)`.** That counts a trailing cell containing no
pixel centre at all whenever the size very nearly covers the frame: 64 pixels in
blocks of 63.9 gives two cells, of which the second is empty. An empty cell has
to get its value from somewhere, so the clamp makes it steal the last pixel from
its neighbour. `sampling::cells` counts the cells that own a pixel instead, which
is the same inequality `blockRange` resolves.

### The block-size control is deliberately in two segments

It is absolute in pixels below 64 of them and relative to the frame above. That
looks like an odd compromise and it is the only mapping that gets both of the
things that have to be true:

- **a format preset is that format at every raster.** A single geometric run
  from one pixel to the whole canvas — the obvious mapping, and what this had
  first — makes "four pixels" four pixels only at the raster it was measured at:
  three at 192 wide, five at 4K. `mbtest --presets` found it by rendering 4:1:1
  and 4:1:0 identically on a small frame, which is what happens when both have
  collapsed onto the same size.
- **the end of the travel is the whole canvas at every raster**, which is the
  headline and cannot be a nominal number.

The kink where the two meet is at the point where the control stops being about
a sampling format and starts being about a mosaic. It is not worth hiding.

Powers of two land on exact twelfths of the travel. That is what makes
`Presets.h` readable, and it is why the preset table is written as `2.0f / 12.0f`
rather than as a decimal.

### Row 0 is the BOTTOM, and that is not arbitrary

`render::apply` takes premultiplied float RGBA with **row 0 at the bottom**,
which is what GL and OpenFX both already are. The lattice is anchored at row 0,
so a block size that does not divide the frame leaves its runt block at
whichever edge row 0 is.

Anchor the CPU render at the top instead and the runt lands at the opposite edge
from the GPU's. `mbtest --cpu` then fails at every non-dividing size, and — much
worse — the same preset lands differently in Resolume and in Resolve.

### `render::apply` is a shipping renderer, not a model of one

It is what the OpenFX plugin renders with. That is why it follows the GPU chain
pass for pass, including the parts that only exist because of how a GPU works:
the same two-stage reduction, means carried with their mean weight rather than
sums, the same partition.

Writing it the obvious way — one loop, gather the block, divide — is perfectly
correct arithmetic and would make `--cpu` useless, because the two would then
differ in the last bits everywhere and the tolerance needed to pass would be
wide enough to hide a real fault.

Means and not sums, specifically: the vertical pass would otherwise be summing
numbers already as large as the frame is tall, and at 4K that is fifteen million
in a 16-bit float whose ceiling is 65504.

### The box mean is alpha-weighted, over unpremultiplied colour

An encoder never sees alpha, so what belongs in the average is the
unpremultiplied colour — weighted by alpha, so a transparent pixel contributes
its area and not its colour, which after unpremultiplying is whatever was in the
buffer.

Average the premultiplied colour instead and every block touching a transparent
edge darkens towards black, which reads as a shadow the plugin invented.
`mbtest --alpha` measures exactly that.

### The exact bypass is what makes `--identity` a real test

When neither lattice is active the composite returns the host's own texel
untouched rather than converting to Y'CbCr and back. Not an optimisation: the
round trip is a matrix and its inverse in float and lands within about 1e-7 of
where it started, and "within 1e-7" is not the same claim as "this effect at
zero is a no-op".

A tolerance wide enough to absorb a matrix round trip is wide enough to hide a
typo in the matrix.

### The audio side cannot be measured from a picture, mostly

`sweep.py` needs `--tone`, which pushes a synthetic spectrum in through the same
call the host makes and drives the clock from a frame counter. Without it all
nine audio controls read dead and the test is right to say so.

Two things about that stimulus are load-bearing and neither is obvious:

- **The three bands must not do the same thing.** Each band is normalised
  against its own recent peak, so a spectrum whose bands rise and fall together
  produces three identical modulation values and the Band dropdowns read dead.
- **The threshold is adaptive**, a multiple of the band's own running flux. A
  train of identical clicks in silence drags the reference down with it and
  fires at every setting, so Sensitivity reads dead. Only a stimulus that is
  always doing something gives the multiplier anything to decide.

And one that is a property of the effect rather than of the test: **Sensitivity
in Step mode cannot be seen in a rendered frame at all.** The latched value is a
running maximum, so as long as the loudest hits fire at every threshold — they
do — changing the threshold only decides whether the quiet ones also fire, and
the maximum is the same either way. `sweep.py` sweeps it in Gate mode;
`mbtest --audio` counts onsets, which is the only place that job is measured.

### GLSL reserved words

`smooth` is an interpolation qualifier, so the reconstruction uniform is
`Reconstruct`. `matrix` is close enough to the sort of word that turns out to be
reserved on somebody's driver, so the colour space uniform is `Space`. Shader
errors surface only at **runtime**, as "the effect does nothing", with a line
number in a file that does not exist.

The GLSL literals are also copied verbatim into a JS template literal by the
demo, so **no backtick and no `${` may appear in them** — including in comments.
`demo/tools/check_shaders.py` says so if one does.

### The SDK's traps, all still live

- `ffglex::ScopedFBOBinding` restores the framebuffer and **not** the viewport.
- Every `ffglex::Scoped*` binding **clears to 0** rather than restoring, so
  allocating an FBO silently unbinds your input texture. Every `Ensure()` is
  called before anything is bound, and `PassBuffer::Ensure` also saves and
  restores `GL_TEXTURE_BINDING_2D` so that stops being a thing an edit can undo
  by moving one line.
- `FFGLScopedFBOBinding.h` is not in the umbrella header.
- `SetParamInfo` clamps a default into 0..1 *before* `SetParamRange` can widen
  it — but only for `FF_TYPE_STANDARD`.
- A display-only TEXT parameter **without** a `SetTextParameter` override makes
  `FF_INSTANTIATE_GL` fail for the whole plugin. The About block is exactly
  that.
- `macroblock_core` is an **OBJECT** library, because `CFFGLPluginInfo`
  registers itself from a file-scope constructor nothing references by name.
- An OBJECT library's objects do **not** travel transitively through a second
  OBJECT library. `macroblock_dsp` is named again on every final target, and
  leaving it off gives a pile of undefined symbols at the last step of an
  otherwise clean build.

### The demo's parameters come through accessors, not properties

`params` in the kit's `render()` is a `Params` instance. Reading
`params.chromaH` off it gives `undefined`, which propagates as NaN into every
block size, makes every lattice inactive, and renders the input untouched with
no error anywhere on the page. It looks exactly like an effect that is switched
off. Use `params.get(id)` and `params.option(id)`.

---

## Relationship to the siblings it sounds like

| sibling | what it is | why this is not it |
|---|---|---|
| **old-cathode** | an analogue television signal path — composite encode, damage, decode | old-cathode's chroma damage is a *consequence* of a subcarrier: bandwidth, cross-colour, dot crawl. This is a **sampling lattice**, which is the digital answer to the same problem and produces blocks rather than smears |
| **regauss** | convergence and geometry errors on a shadow-mask tube | that is a misregistration of three beams. This does not move anything; it reduces how often colour is measured |
| **asciify** | replaces blocks of the picture with glyphs | asciify's blocks are a *rendering* grid, and it destroys the detail inside them. Here the detail is untouched and only the colour is on a grid |

---

## Checking your work

`tools/mbtest` drives the real plugin class through the real FFGL sequence in a
headless CGL 4.1 core context. Time comes from a frame counter, so every run is
deterministic.

| flag | what it proves | what it cannot |
|---|---|---|
| `--partition` | the lattice partitions the axis at 154 size and span combinations. **The test everything else assumes.** | anything about colour |
| `--matrix` | every space's matrix and its inverse are an identity | that the GLSL copy matches — read the `//= mirrored` marks |
| `--mean` | a block's value is the box mean of that block, computed a third way | that the block is in the right place |
| `--cpu` | Resolume and Resolve render the same picture | that both are not wrong the same way |
| `--identity` | the effect at zero is **bit-identical** to its input | that the input really is premultiplied |
| `--constant` | a flat colour survives every lattice and both reconstructions | — |
| `--alpha` | the mean is alpha-weighted, and alpha itself is untouched | — |
| `--full` | the end stop is one chroma value for the whole canvas, and it is the frame's own mean | — |
| `--audio` | silence stays silent, bands are separate, Step latches, Gate snaps, Sensitivity moves the threshold | that any of it feels musical |
| `--presets` | every preset is alive, distinct, and lands on the same block size at 720p, 1080p, 1440p and 4K | that any of them looks good |
| `tools/sweep.py` | every parameter changes the output | that it changes it *correctly* |
| `demo/tools/check_shaders.py` | the demo runs the plugin's own GLSL | that the demo's *ported* arithmetic still agrees |

`sweep.py` is the only thing that catches a mistyped uniform name, since
`glGetUniformLocation` returns -1 and `glUniform(-1)` is a documented no-op — so
a control can be stone dead while everything compiles, links, loads and renders.

---

## What is genuinely verified, and what is assumed

**Verified**, by `tools/verify.sh` on one M4 Max:

- Every invariant in the table above.
- The macOS bundles are universal (checked with `lipo`, not the build log) and
  export `plugMain` / `OfxGetPlugin`, with the registration surviving the link.
- The demo page runs the plugin's shaders character for character.

**Assumed, and not yet checked:**

- **Never rendered through Resolume.** The parameter groups, the dropdowns, the
  FFT buffer parameter and Arena's real texture sizes are all unconfirmed. The
  audio side in particular has only ever seen a synthetic spectrum from the
  harness — whether Resolume's 64 bins behave like it assumes is the single
  biggest open question in the repo.
- **Never opened in an OpenFX host.** Resolve, Nuke and Natron have all not seen
  it. The renderer is verified against the GPU pixel for pixel, so what is
  unknown is the glue: the parameter definitions, the preset handling, and
  whether a host hands over the premultiplication state this expects.
- **Whether the input is genuinely premultiplied.** The chain divides alpha out
  on the way in, and opaque footage looks right either way.
- The Windows build has never been run.
- No performance figure comes from anything but reasoning about tap counts.

---

## Things deliberately not done

Bit-depth reduction on the chroma channels — banding is an amplitude artefact
and this is a spatial one, and the two want separate controls to be legible. A
studio-range clip, which does nothing to content that was already legal. A real
MPEG-2 upsampling FIR rather than bilinear. Per-channel independent lattices
(three grids, and nobody could say what they were looking at). A microphone-
driven demo page, which would need the analyser ported to JS with nothing
checking the port.

---

## Conventions

Tabs. British spelling in prose. Comments explain *why*, and especially what
goes wrong — a comment that restates the code earns nothing. Public MIT repo:
"commit" means commit **and** push.

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
