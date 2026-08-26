"""The demo's GLSL must be the plugin's GLSL, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two have drifted.

------------------------------------------------------------------- the point

`demo/plugin.js` carries a second copy of every shader in `source/shaders/`,
because a browser cannot include a C++ file. Two copies of a shader is exactly
the arrangement that drifts, and the drift is invisible from both sides: the
plugin keeps working, the page keeps working, and they quietly stop being the
same effect. The page's whole claim is that what it runs is the plugin's own
code, so the moment that stops being checkable the page is a lie.

This compares the text, not the behaviour. Reformatting counts as drift, and
that is deliberate -- "it is only whitespace" is how a real change gets waved
through.

--------------------------------------------------------------- what it cannot

Nothing here checks the *ported* arithmetic: `blockSize`, `cellsFor`,
`siteOffset` and `settingsFrom` in plugin.js are a hand translation of
Sampling.cpp and Controls.cpp, and only a reader can tell whether they still
agree. When you change a mapping, change it there too -- and remember that a
wrong one shows up on the page as a block size that is subtly the wrong number
of pixels, which nobody will notice.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The C++ constant, and the JS constant it must equal.
PAIRS = [
    ("source/shaders/Convert.cpp", "kVertex", "VERTEX"),
    ("source/shaders/Convert.cpp", "kConvert", "CONVERT"),
    ("source/shaders/Convert.cpp", "kLattice", "LATTICE"),
    ("source/shaders/Reduce.cpp", "kReduceXBody", "REDUCE_X_BODY"),
    ("source/shaders/Reduce.cpp", "kReduceYBody", "REDUCE_Y_BODY"),
    ("source/shaders/Composite.cpp", "kCompositeBody", "COMPOSITE_BODY"),
]


def cpp_literal(path, name):
    """The body of `const char* const name = R"( ... )";`."""
    source = (ROOT / path).read_text()
    match = re.search(
        r'const char\* const\s+' + re.escape(name) + r'\s*=\s*R"\((.*?)\)";',
        source,
        re.S,
    )
    if match is None:
        return None
    return match.group(1)


def js_literal(source, name):
    """The body of ``const NAME = `...`;`` -- the shaders contain no backticks
    and no `${`, which is asserted below rather than assumed."""
    match = re.search(
        r'^const\s+' + re.escape(name) + r'\s*=\s*`(.*?)`;',
        source,
        re.S | re.M,
    )
    if match is None:
        return None
    return match.group(1)


def main():
    js = (ROOT / "demo/plugin.js").read_text()
    failures = 0

    for path, cpp_name, js_name in PAIRS:
        expected = cpp_literal(path, cpp_name)
        actual = js_literal(js, js_name)

        if expected is None:
            print(f"MISSING  {cpp_name} not found in {path}")
            failures += 1
            continue
        if actual is None:
            print(f"MISSING  {js_name} not found in demo/plugin.js")
            failures += 1
            continue

        # A backtick or a ${ in the GLSL would end the JS template literal early
        # and the mismatch would be reported here rather than at the real cause.
        if "`" in expected or "${" in expected:
            print(f"UNUSABLE {cpp_name} contains a backtick or ${{ -- it cannot be a JS template literal")
            failures += 1
            continue

        if expected == actual:
            print(f"ok       {js_name} matches {cpp_name} ({len(expected)} chars)")
            continue

        failures += 1
        print(f"DRIFTED  {js_name} does not match {cpp_name}")

        expected_lines = expected.splitlines()
        actual_lines = actual.splitlines()
        for i in range(max(len(expected_lines), len(actual_lines))):
            a = expected_lines[i] if i < len(expected_lines) else "<end>"
            b = actual_lines[i] if i < len(actual_lines) else "<end>"
            if a != b:
                print(f"         first difference at line {i + 1}")
                print(f"           {path}: {a!r}")
                print(f"           demo/plugin.js: {b!r}")
                break

    print()
    if failures:
        print(f"{failures} shader(s) have drifted. Copy the C++ across; do not edit the JS.")
        return 1

    print(f"all {len(PAIRS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
