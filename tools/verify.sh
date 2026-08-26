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

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
    local dir bad=0 n=0 shader

    if ! command -v glslc >/dev/null 2>&1; then
        printf '   skipped: glslc not installed (brew install shaderc)\n'
        return 0
    fi

    dir="$( mktemp -d )"

    python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/shaders/Composite.cpp",
	"source/shaders/Convert.cpp",
	"source/shaders/Reduce.cpp",
]

# Shaders the plugin assembles at run time.
# Mirrors composite() in Composite.cpp and reduceX()/reduceY() in Reduce.cpp.
ASSEMBLED = {
	"composite": [ "#version 410 core\n", "kConvert", "kCompositeBody" ],
	"reduceX":   [ "#version 410 core\n", "kConvert", "kLattice", "kReduceXBody" ],
	"reduceY":   [ "#version 410 core\n", "kLattice", "kReduceYBody" ],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# An int indexes the raw strings that are not assigned to a name, in source
	# order. A literal starts with #version. Anything else names a constant
	# above -- and a name that has moved is a KeyError here, not a silent skip.
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

    for shader in "$dir"/*.vert "$dir"/*.frag; do
        [ -e "$shader" ] || continue
        n=$(( n + 1 ))
        if ! glslc --target-env=opengl4.5 -fauto-map-locations \
               "$shader" -o /dev/null 2>"$dir/err"; then
            printf '   %s does not compile\n' "$( basename "$shader" )"
            sed "s|$dir/||; s|^|      |" "$dir/err"
            bad=$(( bad + 1 ))
        fi
    done

    if [ "$n" -eq 0 ]; then
        # No shaders at all is a FAILURE, not a pass. It means the extraction
        # above has lost track of where this repo keeps its GLSL, and a check
        # that silently looks at nothing is worse than no check.
        printf '   no shaders were extracted -- the extraction has gone stale\n'
        rm -rf "$dir"
        return 1
    fi

    if [ "$bad" -eq 0 ]; then
        printf '   %d shaders, all compile\n' "$n"
    fi
    rm -rf "$dir"
    return "$bad"
}

#---------------------------------------------------------------------------
head_ "Shaders"
#---------------------------------------------------------------------------
if shaders_compile; then
    ok "every shader compiles"
else
    bad "a shader does not compile"
fi

# ---------------------------------------------------------------------------
head_ "The release workflow's build options"
# ---------------------------------------------------------------------------
# Every `-DNAME=` the workflow passes must be a CMake built-in or an option this
# project actually declares.
#
# This exists because of a real failed release. The workflow was copied from a
# sibling and renamed with `perl -pe 's/\bTILTER_/MACROBLOCK_/g'` -- and `\b`
# matches nothing between the `D` and the `T` of `-DTILTER_BUILD_FFGL`, so four
# lines kept the old project's option names. CMake does not error on an unknown
# `-D`; it defines an unused cache variable and carries on. The macOS and
# Windows jobs therefore went green while quietly ignoring
# `-DTILTER_BUILD_TOOLS=OFF`, and the Linux job -- whose whole existence depends
# on `BUILD_FFGL=OFF` being honoured -- died trying to build a plugin against a
# submodule it deliberately does not check out.
#
# The tag had to be force-moved. A grep for the old name would have caught it,
# and did not, because both greps run at the time were case-sensitive against
# `tilter`/`Tilter` while the survivor was `TILTER_`.
if [ -f .github/workflows/release.yml ]; then
    declared=$( grep -oE '^[[:space:]]*(option|set)\([[:space:]]*[A-Za-z_][A-Za-z0-9_]*' CMakeLists.txt \
                | grep -oE '[A-Za-z_][A-Za-z0-9_]*$' | sort -u )
    passed=$( grep -oE '\-D[A-Za-z_][A-Za-z0-9_]*=' .github/workflows/release.yml \
              | sed 's/^-D//; s/=$//' | sort -u )
    unknown=""
    for name in $passed; do
        case "$name" in
            CMAKE_*|VCPKG_*|BUILD_OFX) continue ;;
        esac
        if ! echo "$declared" | grep -qx "$name"; then
            unknown="$unknown $name"
        fi
    done

    if [ -z "$unknown" ]; then
        ok "every -D the workflow passes is an option this project declares"
    else
        bad "the workflow passes options this project does not declare:$unknown"
    fi
fi

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
