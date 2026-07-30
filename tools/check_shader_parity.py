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

It also compares *shared struct declarations*, in two directions:

1. native .metal vs the inline fallback string, and
2. native .metal vs the C++ header that declares the CPU-side counterpart
   uploaded via setBytes:/a buffer binding.

Comparing kernel bodies alone was a real blind spot: an audit on 2026-07-27
found `SSAOParams` had lost `float noiseCellSize` in mt_ao.cpp's inline
fallback while every kernel body still referenced `params.noiseCellSize`,
and this script reported MATCH on all of them. Direction 2 matters more than
direction 1 — a CPU/GPU field-order or size mismatch silently misreads every
parameter after the divergence, on the path that actually ships, whereas
direction 1 only breaks the rarely-exercised fallback. Field *order* is
compared, not just the name set, because order is what determines layout.

Struct comparison is deliberately shallow: it matches field types
positionally after mapping C++ spellings onto their MSL equivalents (see
TYPE_ALIASES). It cannot see `alignas`, `packed_*` attributes, or implicit
tail padding, so a clean run is not proof of identical layout -- it only
rules out the drift class that has actually bitten this codebase.
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

# (canonical .metal path, C++ header declaring the CPU-side counterparts of
# its param structs). Only structs declared in *both* files are compared, so
# neither side needs to list which names are shared.
CPU_STRUCT_PAIRS = [
    (
        "src/common/rendering/metal/shaders/native/mt_ao.metal",
        "src/common/rendering/metal/renderer/mt_ao.h",
    ),
    (
        "src/common/rendering/metal/shaders/native/mt_bloom.metal",
        "src/common/rendering/metal/renderer/mt_bloom.h",
    ),
]

SIGNATURE_RE = re.compile(r"\b(?:kernel|vertex|fragment)\s+[\w<>:,\s\*&]+?\b(\w+)\s*\(")
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
STRUCT_RE = re.compile(r"\bstruct\s+(\w+)\s*(?::[^{]*)?\{")

# C++ spelling -> MSL spelling, so a CPU struct written in engine types
# compares equal to its shader counterpart. Sizes must genuinely match:
# every alias here is 4 bytes per component on both sides.
TYPE_ALIASES = {
    "uint32_t": "uint",
    "int32_t": "int",
    "unsigned": "uint",
    "unsigned int": "uint",
    "FVector2": "float2",
    "FVector3": "float3",
    "FVector4": "float4",
}

SCALARS = ("float", "int", "uint", "half", "short", "ushort", "bool", "char", "uchar")
SCALAR_SIZES = {"float": 4, "int": 4, "uint": 4, "half": 2, "short": 2, "ushort": 2,
                "bool": 1, "char": 1, "uchar": 1}

# MSL vector/matrix alignment is the power-of-two-rounded component count, so
# float3 and float4 both align to 16. A C++ counterpart spelled as a plain
# array aligns to its scalar instead -- same size, different alignment.
VECTOR_ALIGN_COMPONENTS = {1: 1, 2: 2, 3: 4, 4: 4}


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


def canonical_type(type_tokens: list) -> str:
    """Map a C++ field type onto its MSL equivalent for comparison."""
    spelling = " ".join(type_tokens)
    return TYPE_ALIASES.get(spelling, spelling)


def footprint(type_spelling: str, field_name: str):
    """(scalar base, component count, natural alignment) for a field.

    Collapses the spellings that describe identical bytes -- `float4x4` and
    `float viewToWorld[16]` both come back as ("float", 16) -- so a CPU struct
    written in scalar arrays still compares equal to its MSL counterpart.
    Returns None for anything unrecognized; callers fall back to comparing
    spellings so an unknown type fails loudly rather than silently passing.
    """
    count = 1
    array = re.search(r"\[(\d+)\]$", field_name)
    if array:
        count = int(array.group(1))

    m = re.fullmatch(r"(" + "|".join(SCALARS) + r")(\d)?(?:x(\d))?", type_spelling)
    if not m:
        return None
    base, vec, mat = m.group(1), m.group(2), m.group(3)
    components = int(vec or 1) * int(mat or 1)
    # An MSL vector aligns to its rounded component count; a scalar array or
    # plain scalar aligns to the scalar. Matrices align like their columns.
    if vec and mat:
        align = SCALAR_SIZES[base] * VECTOR_ALIGN_COMPONENTS[int(vec)]
    elif vec:
        align = SCALAR_SIZES[base] * VECTOR_ALIGN_COMPONENTS[int(vec)]
    else:
        align = SCALAR_SIZES[base]
    return (base, components * count, align)


def field_key(field: str):
    """Comparison key for one "type name" field string."""
    type_spelling, _, field_name = field.rpartition(" ")
    bare_name = re.sub(r"\[\d+\]$", "", field_name)
    fp = footprint(type_spelling, field_name)
    # Alignment is deliberately NOT part of the key: it differs benignly
    # between a vector and an equivalent scalar array. check_alignment()
    # reports the cases where that difference can actually shift an offset.
    return (bare_name, fp[:2] if fp else type_spelling)


def check_alignment(fields: list, label: str) -> bool:
    """Flag fields whose MSL natural alignment forces padding a scalar-array
    CPU counterpart would not insert. Size-equal spellings can still disagree
    on layout, and that is exactly the drift this script exists to catch."""
    offset = 0
    ok = True
    for field in fields:
        type_spelling, _, field_name = field.rpartition(" ")
        fp = footprint(type_spelling, field_name)
        if not fp:
            return ok  # unknown type, offsets past here are unreliable
        base, components, align = fp
        if offset % align:
            print(f"  WARN  {label}: {field} sits at byte offset {offset}, "
                  f"which is not a multiple of its {align}-byte alignment -- "
                  f"MSL will pad, a scalar-array CPU struct will not")
            ok = False
            offset += align - (offset % align)
        offset += SCALAR_SIZES[base] * components
    return ok


def extract_structs(source: str) -> dict:
    """name -> ordered list of "type name" field strings.

    Shallow by design: skips anything that isn't a plain field declaration
    (methods, nested types, static members), since those don't contribute to
    the uploaded layout.
    """
    structs = {}
    text = COMMENT_RE.sub(" ", source)
    for m in STRUCT_RE.finditer(text):
        name = m.group(1)
        body_end = matching_close(text, m.end(), "{", "}")
        body = text[m.end():body_end - 1]
        fields = []
        for decl in body.split(";"):
            decl = " ".join(decl.split())
            if not decl or "(" in decl or "{" in decl:
                continue  # method, initializer, or nested aggregate
            tokens = decl.replace("*", " * ").split()
            if len(tokens) < 2 or tokens[0] in ("static", "constexpr", "using", "typedef"):
                continue
            field_name = tokens[-1]
            fields.append(f"{canonical_type(tokens[:-1])} {field_name}")
        structs[name] = fields
    return structs


def compare_structs(left: dict, right: dict, left_label: str, right_label: str,
                    shared_only: bool) -> bool:
    """Compare ordered field lists. shared_only skips names absent from either
    side (used for .metal-vs-C++-header, where each file legitimately declares
    structs the other has no counterpart for)."""
    ok = True
    if shared_only:
        names = sorted(set(left) & set(right))
    else:
        names = sorted(set(left) | set(right))
    for name in names:
        if name not in left:
            print(f"  FAIL  struct {name}: present in {right_label}, missing from {left_label}")
            ok = False
            continue
        if name not in right:
            print(f"  FAIL  struct {name}: present in {left_label}, missing from {right_label}")
            ok = False
            continue
        if [field_key(f) for f in left[name]] == [field_key(f) for f in right[name]]:
            print(f"  MATCH struct {name} ({len(left[name])} fields)")
            if not check_alignment(left[name], f"{left_label}:{name}"):
                ok = False
            continue
        print(f"  FAIL  struct {name}: field lists differ")
        diff = difflib.unified_diff(
            [f + "\n" for f in left[name]],
            [f + "\n" for f in right[name]],
            fromfile=f"{left_label}:{name}",
            tofile=f"{right_label}:{name}",
            lineterm="\n",
        )
        sys.stdout.writelines("  " + line for line in diff)
        ok = False
    return ok


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

    struct_ok = compare_structs(
        extract_structs(metal_text), extract_structs(fallback_text),
        metal_path.name, f"{cpp_path.name}:{var_name}", shared_only=False)
    return ok and struct_ok


def check_cpu_structs(metal_path: Path, header_path: Path) -> bool:
    print(f"== {metal_path.name} <-> {header_path.name} (CPU-side structs) ==")
    shared = compare_structs(
        extract_structs(metal_path.read_text()),
        extract_structs(header_path.read_text()),
        metal_path.name, header_path.name, shared_only=True)
    return shared


def main() -> int:
    all_ok = True
    for metal_rel, cpp_rel, var_name in PAIRS:
        ok = check_pair(REPO_ROOT / metal_rel, REPO_ROOT / cpp_rel, var_name)
        all_ok = all_ok and ok
        print()
    for metal_rel, header_rel in CPU_STRUCT_PAIRS:
        ok = check_cpu_structs(REPO_ROOT / metal_rel, REPO_ROOT / header_rel)
        all_ok = all_ok and ok
        print()
    if all_ok:
        print("All kernels and shared structs match across canonical, fallback and CPU sources.")
        return 0
    print("Parity check FAILED -- see FAIL lines above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
