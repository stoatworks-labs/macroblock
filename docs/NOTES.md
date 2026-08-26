# Notes

Working notes for this repo: status, decisions, and the traps that have actually
bitten. Written in the first person and dated by when each thing was learned —
that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*Chroma/luma subsampling effect for Resolume Arena/Avenue (FFGL) and OpenFX
(C++/GLSL), ~/Projects/resolume/macroblock, GitHub PUBLIC
(github.com/stoatworks-labs/macroblock), released v0.1.0 on 2026-08-26.*

## 2026-08-26 — built

Effect ID `MB01`, name "Macroblock". An arbitrary chroma subsampling lattice
from a two-pixel pair up to a single chroma value for the whole canvas, with
luma on a second independent lattice. Audio-reactive in Resolume through the
`FF_USAGE_FFT` buffer parameter, with Follow, Step and Gate rules over four
bands.

Built out of the tilter template: CMake MODULE → universal `.bundle`, the same
`Controls`/`Shaders`/`PassBuffer`/`Diag`/`StoatworksAbout` split, `tools/mbtest`
+ `tools/sweep.py` + `tools/verify.sh`, an OpenFX target and a browser demo.

- `tools/verify.sh` is 21 checks and all of them pass on a clean universal
  build. Installed into `~/Documents/Resolume Arena/Extra Effects/`.
- **Never run in Resolume or in any OpenFX host yet.** That is the whole of what
  is unverified and it is a lot — see the bottom of AGENTS.md. The audio side in
  particular has only ever seen a spectrum the harness made up.

## Decisions worth not relitigating

**The reduction is separable and always O(W·H).** Not a performance choice so
much as the reason there is no quality control: the effect costs the same at
every block size, so there is nothing to trade. See AGENTS.md.

**`render::apply` is the OpenFX renderer, not a test double.** That makes
`mbtest --cpu` a statement about the two shipping builds agreeing rather than
about a shader agreeing with a model written to agree with it. It is why the CPU
code follows the GPU chain pass for pass instead of being written the obvious
way.

**No audio in the OpenFX build, and none on the demo page.** OFX has no spectrum
and no clock that would make one meaningful — a timeline renders frames in
whatever order it likes. A browser has no equivalent either, and asking a
visitor for their microphone to demonstrate a video effect is not a trade worth
making. Both say so rather than shipping nine dead controls. No preset touches
an audio parameter, so every menu entry means the same thing in all three
places.

**Bit-depth reduction deliberately left out.** Banding is an amplitude artefact
and subsampling is a spatial one; putting both behind the same controls would
make it impossible to say which one you were looking at.

## Traps that have actually bitten

**`(cell+1)*size` vs `cell*size + size`** — same number, different float. One
pixel in two blocks at a block size of 10.1, around cell 15. Found by
`mbtest --partition`, which is why that test exists and why it sweeps
non-integer sizes.

**`ceil(span/size)` counts an empty trailing cell** at sizes that nearly cover
the frame, and the empty cell steals a pixel from its neighbour. `sampling::cells`
counts cells that own a pixel instead.

**A geometric block-size control is raster-dependent, so a format preset is not
that format.** 4:1:1 and 4:1:0 rendered identically at 192 wide because both had
collapsed onto three pixels. Fixed by making the mapping absolute below 64 px
and relative above; `mbtest --presets` now asserts the four format presets land
on the right block size at 720p, 1080p, 1440p and 4K.

**`sweep.py` reported nine live audio controls as dead**, then three, then one,
and each round was the test rather than the plugin — a harness has no audio, a
settled envelope hides the time constants, three bands that rise together are
one band, and an adaptive threshold fed identical clicks in silence fires at
every setting. The `--tone` stimulus is shaped around all four. The last one,
Sensitivity in Step mode, genuinely cannot be seen in a rendered frame; it is
measured by counting onsets in `--audio` instead.

**The demo's `params` is a `Params` instance, not a plain object.**
`params.chromaH` is `undefined`, which becomes NaN in every block size and
renders the input untouched with no error anywhere. An hour, and it looks
exactly like an effect that is switched off. Use `params.get(id)`.

**A backtick in a GLSL comment breaks the demo**, because the shader text is
copied verbatim into a JS template literal. `check_shaders.py` reports it as
UNUSABLE rather than as a mismatch.

## Fleet issues found on the way

**`stoatworks-backend/resolume-demo/sync.sh` no longer finds any repo.** It
resolves them as `~/Projects/<name>`, and the plugin repos moved under
`~/Projects/resolume/`. It does not fail — it prints `skip <repo> (no demo/)`
and `--check` then cheerfully reports "kit is in sync", having checked nothing.
So the drift guard on every demo page in the fleet is currently switched off.
The kit was vendored into this repo by hand on 2026-08-26 and `macroblock` still
needs adding to that script's `repos` list once the paths are fixed. The backend
repo had uncommitted work in it at the time, so I did not touch it.

**`sync-about.py` has a hard-coded table of repos, and a new one is invisible
until it is in there.** `--only macroblock` answered "unknown repo(s)" rather
than doing anything. The table's own comment records five FFGL repos that
shipped the About block for months while missing from it, so their generated
header could never be refreshed from `projects.json`. Fixed here on 2026-08-26
by adding the row; worth checking first thing on the next new plugin.

## 2026-08-26 — released v0.1.0

Repo public, tagged, CI built, demo deployed to
macroblock-demo.stoatworks-labs.com, website entry and user guide in place.

**Released without ever having run in Resolume or in any OpenFX host.** That was
Allan's call, made knowingly after I flagged it. It is the single biggest thing
this repo does not know about itself, and both the README status table and the
guide's status blockquote say so in as many words rather than burying it.

`sync-about.py` needed `macroblock` adding to its table before it would generate
`StoatworksAbout.h` — the table's own comment says every FFGL repo belongs there
and five had been missing for months, so this is a step to remember rather than
a surprise.

## 2026-08-26 — the release, finished

All six homes agree: repo, website, YouTube (`Ie6mbVMu21k`), both embeds,
downloads, and the Burrow catalogue. Signed and notarised by the launchd agent,
site deployed and verified by content, Instagram reel at
`instagram.com/reel/DcfXslVivry`.

**The first tag failed and had to be force-moved.** The workflow was renamed
from tilter's with `s/\bTILTER_/MACROBLOCK_/`, and `\b` matches nothing between
the `D` and the `T` of `-DTILTER_BUILD_FFGL`, so four lines kept the old
project's option names. CMake does not error on an unknown `-D` — it defines an
unused cache variable — so macOS and Windows went green while ignoring
`-DTILTER_BUILD_TOOLS=OFF`, and the Linux OpenFX job died configuring an FFGL
plugin against a submodule it deliberately does not check out. Both greps run
afterwards were case-sensitive against `tilter`/`Tilter` and never saw `TILTER_`.
`tools/verify.sh` now checks every `-D` in the workflow against the options this
CMakeLists declares.

**Every commit outside this repo went through a clean worktree.**
`stoatworks-backend` was ~250 commits behind its remote with fourteen files of
another session's work in the tree, and `stoatworks-website` acquired another
session's unpushed commit halfway through. Rebasing either would have meant
stashing somebody else's work. For the shared JSON tables — `catalog-data.json`,
`social-*.json` — only the `macroblock` key was carried across, so compander's
and ferric's in-flight entries stayed theirs.

## Still to do

- **Run it in Resolume on real content, with real audio.** Still the first
  thing. The release does not change that, and both the README and the guide say
  so plainly.
- Run the OpenFX build in Resolve.
- **Settle what Resolume's 64 spectrum bins mean in hertz.** `Audio.cpp` splits
  the bands by bin index and `tools/spectrum.py` had to assume a mapping. It is
  now the largest open question, and one Arena answers in a minute.
- `flenser`'s README download block is stale — the unscoped `gen-downloads.py`
  pass corrected its projects.json version from v0.1.1 to v0.1.2 and READMEs are
  skipped by that pass. Left alone because its tree had another session's work.
- `resolume-demo/sync.sh` still needs its paths fixed, and `macroblock` adding
  to its `repos` list afterwards.
