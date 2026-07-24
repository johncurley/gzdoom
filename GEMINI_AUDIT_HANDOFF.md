# Handoff: independent audit of GZDoom's native Metal rendering backend

You are being asked to perform an **independent** code audit. Another AI
assistant (Claude) has already been working in this repository and has its
own findings, but you are not being given those findings — this brief
contains only the context needed to understand the system, not anyone's
conclusions about what's wrong with it. Form your own opinion from the code.
If your conclusions overlap with a prior review, that's a useful
confirmation, not a wasted effort — report everything you find regardless of
whether you suspect it's already known.

**Explicitly avoid** reading `AGENTS.md` in the repo root until *after* you've
completed and written down your own findings. That file is a running session
log of prior debugging work on this exact backend and will bias what you
look for. Everything else described below is fair game and is meant to
orient you, not steer your conclusions.

## What this project is

GZDoom is a modder-friendly Doom-engine source port (C++17, GPLv3) with three
rendering backends: OpenGL, Vulkan (incl. MoltenVK), and a native Metal
backend on macOS. You're auditing the Metal backend specifically. The
checkout is on a branch called `metal-audit` — its explicit purpose is
bringing the Metal backend to correctness/feature parity with the Vulkan
backend (the reference implementation) and evolving it toward a
compute-postprocess / frame-graph architecture. There is **no automated test
suite** for rendering correctness — validation is normally manual (build,
load a WAD, look at it), which is exactly why a careful static/code-reading
audit is valuable here: subtle bugs can sit undetected for a long time.

If you don't have direct read access to this repository's filesystem, ask
the user to paste the specific files you need — don't guess at contents.

## Scope

Audit **all of** `src/common/rendering/metal/` (~14,000 lines across four
subdirectories):

- `system/` — device init, command buffer management, buffer allocation,
  version/capability detection (`mt_version.h`)
- `renderer/` — the state machine, pipeline state, render buffers,
  postprocess/compute modules (AO, bloom). `mt_renderstate.cpp`
  (`MtRenderState`) is the single most load-bearing file in the backend —
  give it disproportionate attention.
- `shaders/` — the GLSL→SPIR-V→MSL translation pipeline glue, plus
  `shaders/native/*.metal` (hand-written native Metal compute kernels for AO
  and bloom — these are compiled directly into a `.metallib`, not translated
  from GLSL)
- `textures/` — texture upload, caching, mipmap generation

Also worth reading for parity comparison (not auditing for its own bugs,
just as the reference to diff Metal's behavior against):
`src/common/rendering/vulkan/system/` (`VulkanRenderDevice` and its
managers — the Metal backend is explicitly designed to mirror this
manager-per-resource-type structure).

**Out of scope**: OpenGL backend, core engine/gameplay code (`playsim`,
`scripting`, save/serialization), anything outside
`src/common/rendering/{metal,vulkan}/`.

**Important**: audit the working tree as it currently stands, not just the
last commit — `git status` / `git diff` will show uncommitted changes on
top of HEAD. Audit what's actually there on disk.

## How to orient yourself (read in this order)

1. `README.md` or root docs for a one-paragraph project overview if you need
   it.
2. `src/common/rendering/metal/README_METAL_RENDERER.md` — Metal renderer
   design notes and an existing Vulkan-vs-Metal parity audit written while
   bootstrapping this backend. Likely somewhat stale (it predates a lot of
   the compute-postprocess work) but good architectural grounding.
3. `.github/copilot-instructions.md` — a field guide to this specific
   codebase's Metal conventions. The "Key Conventions" and "Common Pitfalls"
   sections below are pulled directly from it so you have them without a
   lookup, but the full file has more detail (build commands, debugging
   workflow, file structure reference).
4. `docs/engine-modernization.md` if you want the longer-term architectural
   roadmap (frame graph, deterministic visibility kernel) — background only,
   not required for a correctness audit.

Do **not** read `AGENTS.md` yet (see above).

## Domain knowledge you need before flagging something as a bug

These are intentional, documented design decisions in this codebase.
Don't spend report space rediscovering them — but DO still scrutinize the
code around them for actual mistakes in how they're applied.

- **Coordinate system**: engine-internal is Y-up (OpenGL-style). Metal is
  Y-down, so `MtShaderManager::PatchVertexShader` regex-patches vertex
  shaders at compile time to flip Y and remap Z from `[-1,1]` to `[0,1]`.
  This inversion also flips winding order, which is why Metal's
  `FrontFacingWinding` is deliberately set to **Clockwise** (not the
  seemingly-obvious CullModeBack) to match GZDoom's CW convention under the
  Y-flip. A "winding looks backwards" observation here is very likely
  already-accounted-for, not a bug — but a specific pass that forgot to
  apply this convention would be a real bug.
- **Sampler key values**: `0`=Repeat, `1`=Mirrored Repeat, `2`=Clamp to
  Border, `3`=Clamp to Edge. Seam leaking at texture edges is usually a
  sampler using the wrong one of these, not a UV bug.
- **Ring buffers**: dynamic per-frame GPU data (matrices, lights, animation
  timers) uses circular ring-buffer allocation, 8-16MB/frame. The GPU must
  finish frame N before the CPU overwrites that region on wraparound — look
  for missing synchronization here specifically, this is a real class of
  bug risk, not just a convention to skip past.
- **Lazy state evaluation**: render state (`SetViewport`, `SetScissor`,
  `SetDepthBias`, etc.) is accumulated and only actually applied on
  `Apply()`, immediately before a draw call. Don't flag "state set but not
  applied yet" mid-function as dead code — check whether `Apply()` is
  reachable later in the same logical sequence first.
- **Push constants vs. uniform buffers**: push constants (<256 bytes,
  per-draw, e.g. color/transform overrides) go via `setVertexBytes:` /
  `setFragmentBytes:`; uniform buffers (per-frame, matrices/lighting) go via
  `setVertexBuffer:` / `setFragmentBuffer:`. A CPU-side struct and its
  matching Metal shader struct must match byte-for-byte (size, field order,
  alignment) — this is a real, easy-to-get-wrong bug class worth explicitly
  checking wherever `setBytes`/a struct is passed to a kernel or shader.
  Struct field reordering (someone appending a field "for alignment safety"
  at the end rather than mid-struct) is the kind of thing that's easy to
  get right on one side and forget on the other — check both sides whenever
  you see a shared param struct.
- **Texture caching**: textures are cached by (width, height, format,
  mipmap count); mismatches here cause "flashing" (texture appears/
  disappears frame-to-frame because it's being needlessly recreated).
- **Native `.metal` files are authoritative over inline C++ shader-source
  strings**: several `renderer/mt_*.cpp` files contain an inline string
  (e.g. `SSAO_COMPUTE_SOURCE`, `BLOOM_COMPUTE_SOURCE`) that looks like shader
  source. The **compiled `.metal` files under `shaders/native/`** are what
  actually ships (compiled into `native_shaders.metallib`, loaded first);
  the inline string is only a fallback if the metallib can't be loaded.
  **These two copies can drift silently** — there's a script,
  `tools/check_shader_parity.py`, that diffs kernel bodies between the two
  (comment-stripped) and reports MATCH/FAIL per function. Run it
  (`python3 tools/check_shader_parity.py` from repo root, stdlib-only, no
  deps) as part of your audit and report anything it flags — this is a real,
  previously-observed bug class in this codebase (the two copies have
  drifted before), not a hypothetical.
- **Manager-pattern architecture**: `SystemBaseFrameBuffer` is the abstract
  base; each backend (Vulkan reference, Metal, GL) implements it via one
  manager class per GPU resource type (command buffers, samplers, textures,
  buffers, shaders, pipeline state, render buffers, postprocess). When
  auditing for Vulkan/Metal parity, look for a Vulkan manager whose Metal
  equivalent is missing a feature/fix that exists on the Vulkan side, or
  vice versa.

## Known common pitfall symptoms in this codebase (for pattern-matching, not exhaustive)

| Symptom | Likely cause to check |
|---|---|
| Geometry upside down | `MtShaderManager::PatchVertexShader` not matching all `gl_Position` assignments |
| Seam/line artifacts at texture boundaries | Sampler address mode should be `3` (clamp) not `0` (repeat) |
| Textures flash/pop frame-to-frame | Texture cache key missing a dimension (width/height/format/mip count) |
| Solid black geometry | Uninitialized buffer, invalid texture binding, or a buffer write/read race |
| "Missing texture" placeholders everywhere | Shader translation failure, or stale `.pk3` (needs `zipdir` rebuild) |

## What to actually do

1. Read the codebase directly — don't rely on the docs above as ground
   truth, they can be stale. Treat them as orientation, verify against
   current code.
2. Look for: correctness bugs (wrong math, race conditions, use-after-free/
   dangling pointers around async completion handlers, struct layout
   mismatches between CPU and GPU-side param structs, resource lifetime
   bugs), Vulkan/Metal parity gaps (a fix or feature present in
   `vulkan/system/` missing from the Metal equivalent), and architectural
   concerns (places that violate the manager-pattern separation, or where
   Metal-specific workarounds have accumulated without documentation of
   why).
3. Run `tools/check_shader_parity.py` and include its output.
4. You likely can't build/run this (it's a macOS-only Xcode/CMake C++
   project requiring Metal-capable hardware) — that's expected, this is a
   read/reasoning audit, not a build-and-test one. Say so rather than
   guessing at runtime behavior you can't verify.

## Reporting format

For each finding, give:

- **File and line** (or line range)
- **Category**: correctness / parity-gap / architecture / performance
- **Severity**: your honest assessment — don't inflate minor style
  observations to "critical," and don't undersell something that would
  produce visibly wrong rendering or a crash
- **The concrete failure scenario**: what input/state causes what wrong
  behavior — not just "this looks off." If you can't articulate a concrete
  scenario, say so explicitly and mark it as a lower-confidence
  observation rather than a firm finding.
- **Suggested fix direction** (doesn't need to be a full patch, just the
  shape of the fix)

Rank findings most-severe-first. An empty or short list is a fine outcome if
that's genuinely what you find — don't manufacture findings to fill space.

## After you're done

Once you've written down your own findings, feel free to read `AGENTS.md`
and compare notes — flag explicitly which of your findings are new vs.
already-documented there, since that comparison is the actual point of this
exercise (an independent second opinion, not a race to find the most bugs).
