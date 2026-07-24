#!/usr/bin/env python3
"""Check that GZDoom's Metal native shader sources and their inline C++
fallback-string copies haven't drifted apart.

Background: src/common/rendering/metal/shaders/native/*.metal is compiled
into native_shaders.metallib and loaded first at runtime; the matching
inline raw-string literal in renderer/mt_*.cpp is only a fallback compiled
if the metallib can't be found. Because the fallback path is rarely
exercised, the two copies can silently drift without anyone noticing (this
happened once already, for mt_ao.cpp/mt_ao.metal, fixed 2026-07-10).

This script extracts each named kernel/vertex/fragment function from both
copies of a pair and diffs their bodies (comments stripped, whitespace
collapsed, so header-comment wording differences between the two files
don't false-positive). Run manually after editing either shader source:

    python3 tools/check_shader_parity.py

Exit code 0 if everything matches, 1 otherwise.
"""

import difflib
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# (canonical .metal path, .cpp path holding the inline fallback, name of the
# C++ raw-string variable containing the fallback source)
PAIRS = [
    (
        "src/common/rendering/metal/shaders/native/mt_ao.metal",
        "src/common/rendering/metal/renderer/mt_ao.cpp",
        "SSAO_COMPUTE_SOURCE",
    ),
    (
        "src/common/rendering/metal/shaders/native/mt_bloom.metal",
        "src/common/rendering/metal/renderer/mt_bloom.cpp",
        "BLOOM_COMPUTE_SOURCE",
    ),
]

SIGNATURE_RE = re.compile(r"\b(?:kernel|vertex|fragment)\s+[\w<>:,\s\*&]+?\b(\w+)\s*\(")
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def extract_fallback_source(cpp_text: str, var_name: str) -> str:
    match = re.search(re.escape(var_name) + r'\s*=\s*R"\(', cpp_text)
    if not match:
        raise ValueError(f"couldn't find raw-string literal for {var_name}")
    start = match.end()
    end = cpp_text.index(')"', start)
    return cpp_text[start:end]


def matching_close(text: str, open_pos: int, open_ch: str, close_ch: str) -> int:
    """open_pos points at the char *after* the opening bracket; depth starts at 1."""
    depth = 1
    i = open_pos
    while depth > 0:
        if text[i] == open_ch:
            depth += 1
        elif text[i] == close_ch:
            depth -= 1
        i += 1
    return i  # index just past the matching close_ch


def extract_functions(source: str) -> dict:
    functions = {}
    for m in SIGNATURE_RE.finditer(source):
        name = m.group(1)
        paren_body_start = m.end()  # just past the signature's opening '('
        params_end = matching_close(source, paren_body_start, "(", ")")
        brace_start = source.index("{", params_end) + 1
        body_end = matching_close(source, brace_start, "{", "}")
        functions[name] = source[m.start():body_end]
    return functions


def normalize(body: str) -> str:
    return " ".join(COMMENT_RE.sub(" ", body).split())


def check_pair(metal_path: Path, cpp_path: Path, var_name: str) -> bool:
    metal_text = metal_path.read_text()
    cpp_text = cpp_path.read_text()
    fallback_text = extract_fallback_source(cpp_text, var_name)

    canonical_fns = extract_functions(metal_text)
    fallback_fns = extract_functions(fallback_text)

    ok = True
    names = sorted(set(canonical_fns) | set(fallback_fns))
    print(f"== {metal_path.name} <-> {cpp_path.name} ({var_name}) ==")
    for name in names:
        if name not in canonical_fns:
            print(f"  FAIL {name}: present in fallback, missing from {metal_path.name}")
            ok = False
            continue
        if name not in fallback_fns:
            print(f"  FAIL {name}: present in {metal_path.name}, missing from fallback")
            ok = False
            continue
        if normalize(canonical_fns[name]) == normalize(fallback_fns[name]):
            print(f"  MATCH {name}")
        else:
            print(f"  FAIL  {name}: bodies differ")
            diff = difflib.unified_diff(
                canonical_fns[name].splitlines(keepends=True),
                fallback_fns[name].splitlines(keepends=True),
                fromfile=f"{metal_path.name}:{name}",
                tofile=f"{cpp_path.name}:{var_name}:{name}",
            )
            sys.stdout.writelines(diff)
            ok = False
    return ok


def main() -> int:
    all_ok = True
    for metal_rel, cpp_rel, var_name in PAIRS:
        ok = check_pair(REPO_ROOT / metal_rel, REPO_ROOT / cpp_rel, var_name)
        all_ok = all_ok and ok
        print()
    if all_ok:
        print("All kernels match between canonical and fallback sources.")
        return 0
    print("Parity check FAILED -- see FAIL lines above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
