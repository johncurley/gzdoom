# Handoff: viewpoint uniform consumer contract — 2026-09-27

## Completed

Clarified the existing pointer-free shader-view data boundary without
introducing a scene snapshot or changing frame scheduling:

- `HWViewpointBuffer::SetViewpoint()` now takes
  `const HWViewpointUniforms&`; call sites pass the existing per-view value.
- `HWViewpointUniforms` has a compile-time trivially-copyable assertion,
  matching its byte-for-byte copy into backend data buffers.
- `mUploadIndex` is initialized to zero. Its constructor calls `Clear()`, which
  reads the index; leaving it uninitialized was undefined behavior.
- `docs/audits/audit-render-view-snapshot-linux-2026-09-26.md` records why
  `FRenderViewpoint` and `HWDrawList` are not safe immutable worker packets.
  `docs/engine-modernization.md` points to that audit.

## Verification

On Linux, built both the pointer-API control and const-reference candidate
with `cmake --build build -j$(nproc)`. Both GL and Vulkan MAP06 captures were
reproducible. The paired GL/Vulkan comparison passed in both builds (tone
x1.00; uniform backend noise; median band mean 0.113). Same-backend
control-to-candidate PNG comparisons were identical in the analysis region:
max and mean delta 0 for GL and Vulkan. `git diff --check` passed.

The GL/Vulkan captures establish that this API clarification did not alter
those rendered frames. Linux does not compile the `__APPLE__` body in
`hw_viewpointbuffer.cpp`, so the Metal-specific assignment and mirrored-view
path still need an Apple build check.

## Next work

1. On an Apple build host, compile the Metal renderer to cover the conditional
   `SetViewpoint()` code. No separate Metal implementation is expected: Metal
   uses this shared upload path.
2. Linux flat packets are now implemented for standard indexed flats. They
   retain the existing flat list position and sorting/clipping behavior, carry
   the resolved material/state and current buffer slots, and preserve masked
   and translucent draw branches. Plane/flood hacks, skybox flats, and
   persistent-buffer flats needing dynamic-light uploads remain `HWFlat*`
   fallbacks. The approved `uintptr_t` texture-address token preserves
   `SortFlats()` ordering; `gl_sort_textures 1` passed the GL/Vulkan
   reproducibility check and matched the retained legacy PNGs. See the audit
   for implementation details and limits: the Ashes fixed-view packet-on/off
   A/B matched exactly within the matrix analysis region. A temporary UDMF
   fixture later exercised both masked and translucent packet draws and matched
   a packet-disabled control exactly on GL and Vulkan. The synchronous packet
   is not a cross-frame or worker contract.
3. Keep BSP traversal, render-list production, portals, and simulation/render
   ordering serial until a concrete ownership and lifetime contract exists.

### Linux follow-up — flat packet implementation

The source trace and packet boundary are recorded in the snapshot audit.
`HWFlatDrawState` resolves color/fog values during `AddFlat()`. Standard
indexed flats now use same-frame `HWFlatPacket` values while preserving their
flat-list position; masked/translucent material branches stay equivalent, and
plane/flood hacks, skybox flats, and persistent-buffer flats needing dynamic
lights remain legacy fallbacks. Packet sorting retains the legacy
`FGameTexture*` address order through the approved `uintptr_t` token. The full
build passed; GL/Vulkan baseline and sorted `gl_sort_textures 1` captures
reproduced and matched the retained legacy images. A separate Ashes
packet-on/off A/B matched exactly within the matrix analysis region on GL and
Vulkan. A later static 3D-floor fixture drew both masked and translucent packet
materials on each backend; separate launches reproduced, and packet-on versus
legacy-flat control comparisons had max and mean pixel delta 0 on both backends.
The fixture does not cover every material variant or mixed flat/wall/sprite
ordering. This remains synchronous same-view data, not a cross-frame or
worker-safety guarantee.

## Workspace note

The renderer changes were isolated from the original working tree and committed
as separate viewpoint and flat-packet changes. The original working tree still
contains unrelated ongoing changes and should be preserved.
