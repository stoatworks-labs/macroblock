#!/usr/bin/env bash
#
# Everything that can be checked without a human, in one command.
#
#     tools/verify.sh
#
# ---------------------------------------------------------------- the point
#
# Half of this file checks things the RELEASE job checks. That is deliberate: a
# check that only ever runs in CI, after a tag, is a check that will catch you
# after the tag. The bundle layout, the plist, the architectures and the entry
# points can all be verified here in a second, and the alternative is a failed
# release and a force-moved tag.
#
# The two that have actually bitten this fleet:
#
#   * `CFBundleExecutable` carrying the PREVIOUS plugin's name, because
#     cmake/InfoOFX.plist.in was copied from another repo. Nothing fails: the
#     bundle assembles, the binary is universal, `nm` finds the entry point and
#     a probe renders a correct frame. Then codesign says "code object is not
#     signed at all" and mentions nothing about a plist.
#
#   * A macOS build that is quietly arm64-only, because CMAKE_OSX_ARCHITECTURES
#     was set after the first target existed. The build log calls that a
#     success. Only `lipo` knows.
#
set -uo pipefail

cd "$(dirname "$0")/.."

PASS=0
FAIL=0

ok()    { printf '  \033[32mok\033[0m    %s\n' "$1"; PASS=$((PASS+1)); }
bad()   { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }

# `nm | grep -q` is a RACE under `pipefail`, and the big binary loses: grep exits
# at the first match, nm takes SIGPIPE, and pipefail reports the pipeline as
# failed *because* the symbol was found. Capture first, match afterwards.
symbols_of() { nm -gU "$1" 2>/dev/null; }

# ---------------------------------------------------------------------------
head_ "Build (universal, both plugins)"
# ---------------------------------------------------------------------------
# A fresh configure, because the thing most likely to be stale is the cache that
# decides the architectures.
if cmake -B build -DCMAKE_BUILD_TYPE=Release -UCMAKE_OSX_ARCHITECTURES >/tmp/macroblock-configure.log 2>&1 \
   && cmake --build build >/tmp/macroblock-build.log 2>&1; then
    ok "configured and built"
else
    bad "build failed -- see /tmp/macroblock-build.log"
    tail -20 /tmp/macroblock-build.log
    exit 1
fi

FFGL_BUNDLE="build/Macroblock.bundle"
FFGL_BIN="$FFGL_BUNDLE/Contents/MacOS/Macroblock"
OFX_BUNDLE="build/Macroblock.ofx.bundle"
OFX_BIN="$OFX_BUNDLE/Contents/MacOS/Macroblock.ofx"

# ---------------------------------------------------------------------------
head_ "Architectures"
# ---------------------------------------------------------------------------
for binary in "$FFGL_BIN" "$OFX_BIN"; do
    if [ ! -f "$binary" ]; then
        bad "missing: $binary"
        continue
    fi
    archs=$(lipo -archs "$binary" 2>/dev/null)
    # Both, or Resolume's Intel build and half the installed base cannot load it.
    if [[ "$archs" == *arm64* && "$archs" == *x86_64* ]]; then
        ok "$(basename "$binary") is universal ($archs)"
    else
        bad "$(basename "$binary") is not universal: got '$archs'"
    fi
done

# ---------------------------------------------------------------------------
head_ "Entry points"
# ---------------------------------------------------------------------------
if symbols_of "$FFGL_BIN" | grep '_plugMain' > /dev/null; then
    ok "FFGL exports plugMain"
else
    bad "FFGL does NOT export plugMain -- the host will load nothing"
fi

if symbols_of "$OFX_BIN" | grep '_OfxGetPlugin' > /dev/null; then
    ok "OFX exports OfxGetPlugin"
else
    bad "OFX does NOT export OfxGetPlugin"
fi

# `macroblock_core` is an OBJECT library rather than a STATIC one because
# CFFGLPluginInfo registers itself from a file-scope constructor that nothing
# references by name -- in an archive the linker may drop the whole translation
# unit, and the result is a bundle that loads, exports plugMain, and reports
# that it contains no plugins. The registration's own symbol is the proof it
# survived.
if symbols_of "$FFGL_BIN" | grep 'CFFGLPluginInfo' > /dev/null; then
    ok "the plugin registration survived the link"
else
    bad "no CFFGLPluginInfo in the bundle -- it will report zero plugins"
fi

# ---------------------------------------------------------------------------
head_ "Bundle layout"
# ---------------------------------------------------------------------------
for pair in "$FFGL_BUNDLE:Macroblock" "$OFX_BUNDLE:Macroblock.ofx"; do
    bundle="${pair%%:*}"
    expected="${pair##*:}"
    plist="$bundle/Contents/Info.plist"

    if [ ! -f "$plist" ]; then
        bad "missing: $plist"
        continue
    fi

    actual=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" 2>/dev/null)
    if [ "$actual" = "$expected" ]; then
        ok "$(basename "$bundle") CFBundleExecutable is $actual"
    else
        bad "$(basename "$bundle") CFBundleExecutable is '$actual', expected '$expected'"
    fi
done

# ---------------------------------------------------------------------------
head_ "The lattice and the maths"
# ---------------------------------------------------------------------------
for check in --partition --matrix --audio; do
    if ./build/mbtest "$check" > "/tmp/macroblock${check}.log" 2>&1; then
        ok "mbtest $check"
    else
        bad "mbtest $check -- see /tmp/macroblock${check}.log"
        tail -12 "/tmp/macroblock${check}.log"
    fi
done

# ---------------------------------------------------------------------------
head_ "The picture"
# ---------------------------------------------------------------------------
for check in --identity --constant --mean --alpha --full --cpu --presets; do
    if ./build/mbtest "$check" > "/tmp/macroblock${check}.log" 2>&1; then
        ok "mbtest $check"
    else
        bad "mbtest $check -- see /tmp/macroblock${check}.log"
        tail -12 "/tmp/macroblock${check}.log"
    fi
done

# ---------------------------------------------------------------------------
head_ "No dead controls"
# ---------------------------------------------------------------------------
# The only thing in the repo that catches a mistyped uniform name:
# glGetUniformLocation returns -1, glUniform on -1 is a documented no-op, and a
# control can be stone dead while everything compiles, links, loads and renders.
if python3 tools/sweep.py > /tmp/macroblock-sweep.log 2>&1; then
    ok "every control reaches the picture"
else
    bad "dead controls -- see /tmp/macroblock-sweep.log"
    tail -8 /tmp/macroblock-sweep.log
fi

# ---------------------------------------------------------------------------
head_ "Demo page matches the plugin"
# ---------------------------------------------------------------------------
if [ -f demo/tools/check_shaders.py ]; then
    if python3 demo/tools/check_shaders.py > /tmp/macroblock-demo.log 2>&1; then
        ok "the demo's GLSL is the plugin's GLSL"
    else
        bad "the demo has drifted -- see /tmp/macroblock-demo.log"
        tail -20 /tmp/macroblock-demo.log
    fi
fi

# ---------------------------------------------------------------------------
printf '\n\033[1m%d passed, %d failed\033[0m\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
