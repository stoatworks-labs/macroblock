# macroblock

Chroma and luma subsampling as an FFGL effect for Resolume Arena/Avenue and an
OpenFX effect for Resolve/Nuke/Natron. C++/GLSL, CMake MODULE → universal
`.bundle` (macOS) + Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the lattice, the reduction, or the audio rules.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/mbtest --out /tmp/frame.png`
- List parameters: `./build/mbtest --list`
- Render footage: `ffmpeg ... -f rawvideo - | ./build/mbtest --pipe --size WxH --script cue.txt --spectrum s.txt | ffmpeg ...`
- A spectrum from a track: `python3 tools/spectrum.py track.wav --fps 60 -o s.txt`

## OpenFX build
Built by default; copy `build/Macroblock.ofx.bundle` to `/Library/OFX/Plugins`.
Disable with `-DBUILD_OFX=OFF`.

## Verify
- Everything: `tools/verify.sh`
- The lattice partitions the axis: `./build/mbtest --partition`
- A block carries its own box mean: `./build/mbtest --mean`
- Resolume and Resolve agree: `./build/mbtest --cpu`
- No dead controls: `python3 tools/sweep.py`
- The demo has not drifted: `python3 demo/tools/check_shaders.py`

## Notes
- **The reduction is separable, and that is load-bearing.** Reduce X reads every
  source pixel exactly once; reduce Y finishes the column. Total work is O(W·H)
  at every block size, so the effect costs the same at 2 px and at whole-canvas
  and there is no quality control to offer. Gathering the block in one pass is
  the same total and puts eight million taps in one fragment.
- **`blockRange` must partition the axis.** Every pixel in exactly one cell, at
  any real block size. `mbtest --partition` is the only thing checking it.
- **A cell's end is `(cell+1)*size`, never `cell*size + size`.** Equal in
  arithmetic, not in float; the two disagree in the last bit and a pixel lands
  in two blocks.
- **`cells` is not `ceil(span/size)`** — that counts a trailing cell owning no
  pixel, which then steals one from its neighbour.
- **The block-size control is absolute below 64 px** and relative to the frame
  above it. That is what makes a format preset that format at every raster.
  Powers of two land on exact twelfths of the travel.
- **Row 0 is the BOTTOM** in `render::apply`, matching GL and OpenFX. The
  lattice is anchored there, so flipping it moves the runt block to the other
  edge and Resolume and Resolve stop agreeing.
- **The box mean is alpha-weighted**, over unpremultiplied colour. Averaging
  premultiplied colour darkens every block on a transparent edge.
- **The composite bypasses exactly** when neither lattice is active, so
  `--identity` is a bit-exact test rather than a tolerance.
- **`render::apply` is the OpenFX renderer**, not a test double. `--cpu`
  comparing it to the GPU is "the two shipping builds agree".
- **Audio runs before the lattice**, every frame. The other order costs a frame
  of latency between the hit and the blocks.
- `smooth` is a GLSL interpolation qualifier — the uniform is `Reconstruct`. The
  matrix uniform is `Space` for the same class of reason.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can
  widen it; options are exempt.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no
  host can instantiate the plugin at all.
- `macroblock_core` and `macroblock_dsp` are OBJECT libraries, and an OBJECT
  library's objects do **not** travel transitively through a second one — every
  final target names `macroblock_dsp` itself.
- macOS build must be universal. Verify with `lipo`, never the build log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. `~/Library/Logs/macroblock/`.
