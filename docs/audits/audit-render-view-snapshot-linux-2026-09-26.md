# Render-view snapshot boundary audit — Linux — 2026-09-26

## Purpose

Inspect the next Track A step, an immutable render-world snapshot, against the
current scene path before proposing code changes. This is a source audit only:
the Linux GL/Vulkan runtime and shared frame graph are unchanged.

## Finding

`FRenderViewpoint` is a mutable per-view traversal context, not an immutable
world snapshot. `HWDrawInfo::StartScene()` copies it by value, but the copy
retains raw pointers to `player_t`, `AActor`, `FLevelLocals`, and `sector_t`
(`src/rendering/r_utility.h`, `FRenderViewpoint`; `hw_drawinfo.cpp`,
`StartScene`). A shallow copy therefore does not isolate the view from live
gameplay state.

The scene collector follows those references directly. `HWDrawInfo::CreateScene()`
calls `RenderBSP(Level->HeadNode(), ...)`, maps backend vertex/light/bone
buffers, and then runs missing-texture and render-hack passes. Its own comment
states those hacks cannot be multithreaded because they use global `validcount`
(`src/rendering/hwrenderer/scene/hw_drawinfo.cpp`, `CreateScene`). The BSP and
render-hack implementations read and write `validcount` plus per-sector,
subsector, linedef, and actor visitation fields.

The context is also modified across views. `RenderViewpoint()` applies stereo
eye offsets to a copied `HWDrawInfo::Viewpoint`; portal code copies and then
transforms viewpoints; OoB/portal code can change the camera actor's render
flags. `DrawScene()` has function-static recursion and SSAO-portal counters.
Those behaviors mean an immutable value record must be produced per resolved
view (main, camera texture, eye, and portal), not once as a frame-global camera
snapshot.

## Contract correction

Split the roadmap phrase “render-world snapshot” into two separate concepts:

1. **Render-view values:** owned numeric and enum state for one resolved view,
   such as position, angles, projection/view transforms, interpolation fraction,
   and view flags. It must not contain raw gameplay pointers, borrowed spans,
   callbacks closing over live actors, or backend resource handles. Portal and
   stereo transforms produce distinct values. Existing consumers remain serial
   until they are explicitly migrated to this type.
2. **Scene data ownership:** geometry, sectors, actors, lights, portal topology,
   material references, and render flags needed by scene collection. This is a
   separate, larger contract. No worker may read these through live gameplay
   pointers while simulation can mutate them. It needs either owned render data
   or an explicit lifetime/synchronization scheme, plus removal or isolation of
   shared visitation state such as `validcount`.

Do not introduce a duplicate view struct without a consumer. The existing
`FRenderViewpoint` already provides a serial per-view copy; the first code
increment should be selected only after identifying a narrow consumer that can
use a pointer-free value record without changing portal, stereo, camera-texture,
or interpolation behavior.

## Existing value-data consumer

The narrow GPU-view consumer already has a suitable value object:
`HWViewpointUniforms` (`src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h`).
Its matrices are inline `VSMatrix` arrays and its remaining members are numeric
scalars/vectors; it contains no gameplay or backend pointers. `SetupView()`
finishes the current view's matrices and camera position, then calls
`HWViewpointBuffer::SetViewpoint()`. That function copies the struct into the
backend data buffer before binding the resulting upload index. On Metal it
also stores a value copy in `mLastSceneViewpoint` for later depth reconstruction.
Portal view setup reaches the same viewpoint binding path after transforming
the child view.

This is already a synchronous, pointer-free handoff for shader uniforms. It
does not snapshot the BSP, visible actors, sectors, lights, portal graph, or
material state consumed by `CreateScene()`. Treat it as the first verified
value-data boundary, not as a scene snapshot or as evidence that scene
collection can move to a worker. No new wrapper around `HWViewpointUniforms`
is justified until a second consumer needs a distinct owned view-data contract.

## Implemented boundary clarification

`HWViewpointBuffer::SetViewpoint()` now accepts `const HWViewpointUniforms &`
instead of a pointer. This makes the existing read-only, synchronous consumer
contract explicit at each call site without changing the upload layout,
backend binding, or per-view setup order. A compile-time assertion requires
`HWViewpointUniforms` to remain trivially copyable because it is copied
byte-for-byte into the backend buffer.

The build exposed a pre-existing initialization defect at this exact consumer:
`HWViewpointBuffer` called `Clear()` in its constructor, and `Clear()` read
`mUploadIndex` before it had been initialized. The member now starts at zero,
which is the initial upload state `Clear()` expects. This is required for the
constructor path to have defined behavior; it does not alter the subsequent
pipeline rotation rule.

Verification on Linux used a pointer-API control build and the const-reference
candidate build, both with the zero-initialized upload index. `cmake --build
build -j$(nproc)` succeeded for both. The GL/Vulkan `doom2`/MAP06 capture
self-check was reproducible on both backends in both builds; paired captures
reported `OK` (tone x1.00, uniform backend noise, median band mean 0.113).
Same-backend control-to-candidate PNG comparisons were byte-identical in the
analysis region: GL max delta 0 and mean delta 0; Vulkan max delta 0 and mean
delta 0. This verifies the API clarification did not change the observed
rendered frame. It does not validate Metal on this Linux machine, or make scene
collection safe to parallelize.

The obvious next collection product, `HWDrawList`, is not a safe packet boundary
yet. Its arrays hold arena-allocated `HWWall`, `HWFlat`, and `HWSprite` objects;
those objects retain pointers to live geometry, sectors, actors, particles,
textures, light lists, and portal data (`hw_drawlist.h` and `hw_drawstructs.h`).
Copying the arrays would copy those borrows, not make the packet immutable.
Before extracting a packet, select one narrow draw class and trace the exact
fields its draw method reads, including material and light lifetime. The
existing `HWViewpointUniforms` path is therefore the bounded, verified value
slice; the draw lists need a separate ownership design before conversion.

## First draw-class trace: `HWFlat` — Linux source audit, 2026-09-27

`HWFlat::DrawFlat()` is a useful bounded trace, but the whole class is not a
safe packet today. The ordinary opaque route reads its stored light/color
values, `HWSectorPlane`, `texture`, `TextureFx`, `sector`, `section`,
`iboindex`, and `dynlightindex`; it also reads per-view `HWDrawInfo` state and
mutates `FRenderState`. `DrawSubsectors()` may rebuild lights at draw time from
`Level->lightlists.flat_dlist`, checking each live `FDynamicLight` before
uploading a new light buffer. A shallow copy therefore keeps live sector,
texture-manipulation, section, texture, and level-light references, and does
not freeze the light result.

The class combines materially different draw paths: ordinary flats, skybox
planes, translucent flats, missing-texture plane hacks, and flood-plane stencil
draws. The two hack paths walk per-view lookup tables whose linked nodes retain
`subsector_t*`/`seg_t*` references and renderer vertex indices. The skybox path
uses a separately allocated vertex range. A single packet representation
cannot treat these as one uniform payload without first preserving those
branches and their state changes.

If `HWFlat` remains the first packet candidate, use the standard `DrawFlat()`
route across existing lists and retain skybox, plane-hack, and flood-hack paths
on the serial legacy route. Before implementation, define same-frame lifetimes
for the referenced texture/material, section geometry and index buffers, and
uploaded light range; copy the plane texture transform inputs or final
transform. This is only a candidate boundary, not evidence that those
lifetimes support deferred or worker-thread consumption. No renderer code was
changed by this source trace.

### Same-frame lifetime check — Linux source, 2026-09-27

- The `HWFlat` records are allocated from the global `RenderDataAllocator`.
  `HWDrawInfo::EndDrawInfo()` resets the draw lists, but frees that arena only
  after the outermost view ends (`gl_drawinfo == nullptr`). Nested portal views
  therefore share the arena. This gives a same-render-view-stack lifetime,
  not ownership that can safely escape the view stack.
- Flat geometry is built from level-owned `FSection` ranges. The indices are
  `iboindex + section->vertexindex` and `section->vertexcount`; `FSection`
  resides in `FLevelLocals::sections` and is cleared on level teardown. The
  vertex data is appended to the active `screen->mVertexData` buffer after its
  per-view reset, then uploaded on unmap. The range is meaningful only with
  that active buffer generation. Neither the raw `section` pointer nor the
  bare indices alone identify a durable packet resource.
- `FLightBuffer::Clear()` selects the frame's pipeline buffer and resets its
  append index. `UploadLights()` copies light values into that buffer and
  returns the starting vec4 index; `SetLightIndex()` consumes it during draw.
  The index is therefore useful only with the matching active light buffer
  and before its next clear/rotation. On the GL uniform-buffer fallback the
  render state further maps the logical index to a binding range at draw time.
- `TextureFx` points into level sector/plane state, while `Texture` is resolved
  through `TexMan`. A value packet can copy `TextureManipulation` and retain a
  stable texture identifier, but lifetime and animation resolution still need
  to match the existing per-view material selection. The current record does
  neither: it borrows `TextureFx` and `FGameTexture*`.

This closes only the resource-lifetime question for immediate same-frame
consumption: geometry and uploaded-light references have identifiable active
buffer generations, and the draw-list arena spans nested views. It does not
establish a cross-frame or worker-thread contract. Any packet implementation
must carry or resolve the matching vertex/light buffer generation and copy
plane manipulation values; it must not retain pointers into the draw-list
arena or level-owned section/sector arrays. The scope is still source-audited
only, with no renderer code change or runtime claim.

## Approved candidate contract — standard `HWFlat` draws

The user approved preserving masked and translucent material behavior. This
replaces the earlier “opaque flats” shorthand: packet eligibility must not be
inferred from `GLDL_PLAINFLATS` or `DrawFlat(..., false)`. `AddFlat()` routes
some texture-translucent surfaces to the plain list, while the masked and
translucent lists also carry material-specific alpha semantics.

### Scope

The candidate is a value record for an `HWFlat` dispatched through the normal
`HWFlat::DrawFlat()` path. It includes masked and translucent materials and
preserves the list/pass selected by `HWDrawInfo::AddFlat()`. `hacktype` plane
and flood paths, and the skybox-specific four-vertex/clamp path, remain on the
legacy path in this first scope. `AddFlat()`'s list classification remains the
authority; the packet consumer must not reclassify a surface based on an
“opaque” label.

### Required record data

The packet is consumed during the same render-view stack in which it is
produced. It contains values or explicit frame-local resource references, not
`HWFlat*`, `sector_t*`, `FSection*`, a `TextureManipulation*`, or pointers into
`RenderDataAllocator`.

- **Geometry:** the resolved indexed draw range (the current
  `iboindex + section->vertexindex` and `section->vertexcount`), plus identities
  for the active vertex and index buffer generations. Do not retain the level
  `FSection*` or assume an integer offset identifies the right active buffer.
- **Material and alpha:** the exact texture selected for this view/frame,
  including invalid/no-texture state; masked/translucency properties; the
  selected list/pass; render style and alpha; texture clamp/translation choices;
  and a resource reference that remains valid until consumption. Preserve the
  existing `gl_mask_threshold` versus zero-threshold branch in the translucent
  draw path, and the pass-level alpha threshold for masked lists.
- **Surface values:** plane normal, the inputs or final value of the plane
  texture transform, `TextureManipulation` by value (or an explicit no-op),
  colormap/fog inputs, flat/add colors, light level, fullbright/light-mode
  result, and dither-transparency state. Resolve current-view extra light and
  any `FLevelLocals`/portal fog inputs before publication; the packet cannot
  consult those live globals later.
- **Lighting:** a frame-local light-buffer generation and index, or owned
  backend-neutral light values that the backend uploads before draw. Keep
  dynamically lit flats on persistent-buffer backends on the `HWFlat*` legacy
  path for the first packet increment. `DrawSubsectors()` rebuilds and uploads
  their lights during drawing, and the bounded light buffer makes upload order
  observable; retaining that path preserves its ordering across plain/masked
  passes and the mixed translucent sort. On nonpersistent-buffer backends,
  `PutFlat()` already resolves and uploads lights during collection. A packet
  may carry that existing light-buffer slot/index without re-uploading or
  reordering it. Do not defer reads through `Level->lightlists.flat_dlist` or
  retain a `FDynamicLight*`.
- **Per-view translucent adjustment:** preserve the current plane-normal-based
  z shift and corresponding viewpoint-uniform update/restore around the draw.
  Encode it as an explicit scoped draw adjustment; do not mutate a shared
  viewpoint while producing packets.

### Ordering and consumer rules

The existing draw-list classification and traversal remain authoritative.
Solid-list, masked-list, and translucent-list packets stay in their current
phases with their current inherited depth, alpha, and blend state. The
`GLDL_TRANSLUCENT` list is sorted together with walls and sprites and uses flat
height for clip-split decisions; a flat packet must remain a `DrawType_FLAT`
entry in that same mixed sort input, carrying the sort values the current code
reads. The final sorted traversal also determines when persistent-buffer
dynamic lights are uploaded. `GLDL_TRANSLUCENTBORDER` retains insertion order. Optional
`gl_sort_textures` ordering for the plain/masked lists must be resolved with the
same legacy rules before consumption. The packet work introduces no new sort,
batch reorder, frame-graph node, scheduling change, or worker execution.

### Gate before implementation

This is a behavior contract, not a claim that the current backends expose
these resource references cleanly. Before coding, trace the backend-neutral
way to identify paired vertex/index and light-buffer generations, and the
material resolver that preserves the exact animated texture selected during
`Process()`. If either needs a live sector/renderer pointer to resolve later,
the contract is not met and the implementation must stop for a narrower
interface. Any implementation then needs same-view captures with a failing
control and a fixed-scene comparison covering plain, masked, and translucent
flat examples, including a mixed translucent flat/wall/sprite order case.

### Resource-handle feasibility check — Linux source, 2026-09-27

- `FFlatVertexBuffer` already exposes its active pipeline slot through
  `GetPipelinePos()`. Its index buffer is a single static buffer owned by that
  object; the sector/3D-floor index values identify ranges within it. The
  active vertex slot plus those integer ranges can identify geometry within
  the synchronous render-view scope, provided the consumer checks against the
  same `screen->mVertexData` owner. Do not store its `IVertexBuffer*` or
  `IIndexBuffer*` in the packet.
- `FLightBuffer` uses the same pipeline-slot pattern, but `mPipelinePos` is
  private and the only public buffer accessor returns `IDataBuffer*`. The
  smallest missing common API is a read-only `GetPipelinePos()` accessor; a
  packet stores the logical slot and light index, and the consumer verifies
  them against the active `screen->mLights`. This is sufficient only because
  packets are consumed before the next `Clear()`/rotation, inside the same
  render-view stack. The existing light-index API remains responsible for
  backend binding details.
- `HWFlat::Process()` resolves animation with
  `TexMan.GetGameTexture(plane.texture, true)`. The returned `FGameTexture`
  exposes its resolved ID through `GetID()`, and
  `TexMan.GetGameTexture(resolvedID, false)` retrieves that exact entry without
  applying animation a second time. The packet can store the resolved
  `FTextureID`, including the invalid/no-texture case, rather than borrowing
  `FGameTexture*`. `FTextureID` is a table index without a generation, so this
  is valid only while the texture table remains unchanged during packet
  consumption. The inspected mutation paths (`SortTexturesByType()` during
  resource construction, `DeleteAll()` during manager init/teardown, and
  `FlushAll()` cleaning hardware data) do not mutate the table during the
  synchronous draw path; keep packet lifetime inside that path.
- `TextureManipulation` contains only `PalEntry` values and a float, so its
  value can be copied directly from the sector/plane at packet creation. No
  ownership wrapper is needed for that field.

**Feasibility result:** a backend-neutral, same-render-view packet can name
these resources without carrying backend pointers, using the existing vertex
slot, one small light-slot accessor, the active screen buffer context, and the
resolved `FTextureID`. This does not support retention after the draw-list
arena is reset, after buffer rotation, across texture-table rebuilds, or on a
worker. If implementation proceeds, add the light-slot accessor together
with its first consumer (not as an unused API), and assert that both packet
slots match the currently active buffer owners before drawing.

### Persistent-light fallback decision — 2026-09-27

The user selected the legacy fallback for persistent-buffer dynamic lighting:
those flats remain `HWFlat*` entries and continue calling `SetupLights()` from
`DrawSubsectors()`. They are ineligible for the first packet path. This is
necessary because `PutFlat()` uploads lights in collection order only on
nonpersistent-buffer backends, while persistent-buffer backends upload them in
draw order; the fixed-capacity buffer can make a changed order affect which
lights are retained. Nonpersistent backends may packetize a flat only after
preserving the light slot/index already produced by `PutFlat()`.

The existing renderer is currently the legacy implementation, so there was no
new fallback branch to toggle. A Linux GL control run through
`tools/matrix/run.py --scene doom2 --only baseline` produced three distinct
pixel hashes (`4b46f461`, `bae353bb`, `97977a8e`); this route was not a stable
control. The repository's cross-backend capture path is the reliable one:
`tools/matrix/crossbackend.py --only baseline --scene doom2 --selfcheck --keep
--backends gl,vulkan` passed, with repeatable GL (`mean=21.224`) and Vulkan
(`mean=21.278`) captures. Use that command as the broad legacy control. It does
not isolate masked, translucent, or dynamically lit flat paths, so a dedicated
flat-path comparison is still required before claiming equivalence for those
cases.

`SetFog()` in `src/rendering/hwrenderer/scene/hw_setcolor.cpp` reads
`FLevelLocals` fog fields, `portalState.inskybox`, `gl_fogmode`, and colormap
state when drawing a flat; `GetFogDensity()` also reads the level's outside-fog
color and density. The packet must not carry those live inputs. Resolve them
to a compact fog/light-state value during collection and apply that value at
draw time. The value/API shape is now implemented for standard flats; see
“Resolved flat color/fog state” below. The flat record itself is still the
legacy `HWFlat` object, so this closes the consumer-state gap without claiming
that the full packet conversion is complete.

### Draw-list dispatch boundary — Linux source, 2026-09-27

The flat packet has to travel through `HWDrawList` as the same ordered flat
item. `HWDrawItem` currently stores a `DrawType_FLAT` tag and an index into the
`flats` pointer array. That entry is consumed in all of these behaviors:

- plane discovery and plane-to-plane ordering (`FindSortPlane()`,
  `SortPlaneIntoPlane()`);
- wall/sprite splitting against a flat's height (`SortWallIntoPlane()`,
  `SortSpriteIntoPlane()`);
- optional texture sorting (`SortFlats()`);
- regular and sorted drawing (`DoDraw()`, `DrawFlats()`, `DrawSorted()`),
  including translucent clip-split restoration from the flat height.

Therefore, adding a separate top-level `DrawType_FLAT_PACKET` would make the
current flat-specific branches miss packet items unless every one of these
sites learned a second tag. Keep `DrawType_FLAT` and the `drawitems` order as
the authority. The flat payload slot needs an internal tagged choice between a
legacy `HWFlat*` and a value packet, with shared accessors for the sort values
(at minimum z, ceiling, and the exact legacy texture-sort order) and draw dispatch. Wall/sprite splitting,
masked-list sorting, mixed translucent ordering, and clip planes continue to
use that common flat view. This is a required dispatch detail of the approved
contract, not an invitation to add a new sort phase.

Source anchors: `src/rendering/hwrenderer/scene/hw_drawlist.h` declares the
parallel object arrays and `HWDrawItem`; `src/rendering/hwrenderer/scene/
hw_drawlist.cpp` contains the sort and draw consumers listed above. No code
change is made in this source trace.

### Flat texture-sort ordering constraint — Linux, 2026-09-27

`HWDrawList::SortFlats()` compares `HWFlat::texture` with raw pointer `<`
(`hw_drawlist.cpp`, `SortFlats()`). This orders by the resolved
`FGameTexture*` address; it is not an `FTextureID` comparison. A packet that
stores only the resolved texture ID can therefore change `gl_sort_textures`
ordering, even when it resolves the same material. The approved packet
contract requires preserving the legacy order, so replacing that key with
`FTextureID` would be a separate behavior change requiring its own measurement.

The user approved the minimal correction: each packet captures a `uintptr_t`
sort token from the exact resolved texture object at collection. Sorting uses
that token for packet entries and the equivalent conversion for legacy entries.
The token is ordering data only; material lookup still uses the resolved
`FTextureID`, and the packet never dereferences the token. A debug assertion
checks that the token order agrees with the legacy pointer comparator.

### Resolved flat color/fog state — Linux, 2026-09-27

`HWFlatDrawState` now stores the computed color, alpha, desaturation,
software-light setting, fog color/density, and any fog-specific light
parameters. `HWDrawInfo::AddFlat()` resolves and stores it on the copied
`HWFlat` while the current level, portal, and view context are available.
`HWFlat::DrawFlat()` applies only those stored values; it no longer queries
extra light or calls the level/portal-reading `SetColor()`/`SetFog()` helpers.
The helpers remain available to other draw classes. Standard indexed flats now
use a value packet with the same draw-list entry position; masked and
translucent branches keep their existing material, alpha-test, render-style,
and viewpoint-shift logic. Plane/flood hacks, skybox flats, and
persistent-buffer flats needing dynamic lights retain the legacy `HWFlat*`
fallback. Draw-list sorting, clipping/splitting, and mixed translucent order
continue to consume the common `DrawType_FLAT` entry. Packet metadata includes
the current vertex/light buffer slots, exact indexed range, resolved material
ID, and legacy texture-address sort token.

Verification on the Linux RX 550: `cmake --build build -j$(nproc)` passed.
The baseline GL/Vulkan MAP06 self-check reproduced both backends (GL mean
21.224, Vulkan mean 21.278). The same check with `gl_sort_textures 1` also
passed on both backends, exercising the pointer-token sorting path; the source
also retains a debug equivalence assertion against pointer ordering. The
sorted-run 640×480 PNGs were byte-identical to the
retained legacy controls for both GL and Vulkan. `git diff --check` passed.
Masked and translucent branches are preserved by the packet draw implementation
and share their original list/order logic; the stock MAP06 capture does not by
itself establish coverage of every masked/translucent material variant.

### Packet-on versus legacy-flat A/B — Linux, 2026-09-27

To isolate packet dispatch, the packet eligibility condition was temporarily
disabled while keeping the resolved `HWFlatDrawState` change in both arms.
Using the Ashes Hard Reset `save01.zds` viewpoint, both the fallback build and
the packet build passed GL/Vulkan self-checks. Same-backend comparisons over
the analyzed 640×480 region were exact: GL max/mean delta 0, Vulkan max/mean
delta 0. Final packet build self-check means were GL 24.852 and Vulkan 24.904.

A temporary stderr probe in `HWFlatPacket::DrawFlat()` reported when a packet
with a masked texture or translucent material/alpha was actually consumed. It
reported neither type in the savegame view. A diagnostic sweep of Ashes Hard
Reset maps MAP01–MAP14, plus Ashes Afterglow MAP14, also found no such packet
draws. A fresh Ashes MAP01 start was not repeatable (GL/Vulkan capture means
varied by about 0.002 between its two launches), so it was excluded from image
comparison. All temporary probes and matrix-config changes were removed, and
the final production build and `git diff --check` passed.

The A/B supports equivalence for the material paths visible in the stable
savegame scene; it does not establish masked or translucent flat equivalence.
Those branches remain implemented according to the traced legacy alpha and
render-style rules; the fixed-scene coverage gate was closed by the follow-up
below.

### Masked/translucent packet fixture — Linux, 2026-09-28

A temporary UDMF MAP01 fixture placed the player in a 512x512 room beneath
two visible 3D floors. One used a static Doom II flat with alpha 128; the other
used an RGBA texture with alternating opaque and transparent rows at alpha 255.
The two control sectors used `Sector_Set3DFloor` (special 160) and targeted the
room's sector tag. The fixture lived under `/tmp`, outside the repository.

A temporary probe inside `HWFlatPacket::DrawFlat()` printed once for each
masked and translucent material path. Both markers appeared on OpenGL and
Vulkan. The matrix harness captured each backend twice with identical PNGs;
repeating the capture in a separate invocation also gave GL max/mean delta 0.
The first fixture used animated `FWATER1` for the translucent plane and changed
between invocations, so it was replaced by a static flat before comparison.

For the control, packet selection in `AddFlat()` was temporarily disabled while
leaving `HWFlatDrawState` resolution active. The control emitted neither marker
and reproduced on both backends. Comparing packet-on and control screenshots
within the harness's 640x480 image analysis box `(32,43)-(608,384)` gave
**max delta 0, mean delta 0** on both GL and Vulkan. GL versus Vulkan on this
fixture exceeded the harness's cross-backend threshold, so this result claims
same-backend packet equivalence, not GL/Vulkan image parity. The probes,
selection override and temporary scene configuration were removed before the
final `cmake --build build -j$(nproc)` gate, which passed.

This covers one masked and one translucent 3D-floor draw route. It does not
exercise every material variant or a mixed flat/wall/sprite ordering scene.

## Safe boundary for future work

- Keep `HWDrawInfo::CreateScene()`, BSP traversal, portals, and render-list
  production serial and on their current thread.
- Treat `FRenderViewpoint` and `HWDrawInfo` as traversal state, not as a thread
  handoff object.
- A future value-only view record can describe inputs for a consumer, but it
  does not by itself make scene collection parallel or prove that referenced
  gameplay data stays alive.
- Before worker scene collection, inventory every read/write of world data and
  global/per-object visitation state. Define ownership and lifetime for each
  record, then compare output against the current serial path on a fixed scene.
- Keep the frame graph diagnostic-only; this audit does not authorize graph
  scheduling, resource aliasing, or simulation/render concurrency.

## Source anchors

- `src/rendering/r_utility.h`: `FRenderViewpoint` fields, including gameplay
  pointers alongside value fields.
- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`: per-view shallow copy in
  `StartScene()`, buffer mapping and `RenderBSP()` in `CreateScene()`, explicit
  `validcount` multithreading warning, and static recursion state in `DrawScene()`.
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` and
  `hw_viewpointbuffer.{h,cpp}`: the pointer-free uniform record, trivial-copy
  contract, explicit const-reference consumer, and synchronous copy/bind path.
- `src/rendering/hwrenderer/scene/hw_drawlist.h` and `hw_drawstructs.h`: the
  draw-list index records plus pointer-bearing wall/flat/sprite payloads.
- `src/rendering/hwrenderer/hw_entrypoint.cpp`: frame setup, one draw-info per
  eye, stereo view offset, and scene processing.
- `src/rendering/hwrenderer/scene/hw_portal.cpp`: portal transforms applied to
  viewpoint copies and actor render-flag mutation.
- `src/rendering/hwrenderer/scene/hw_bsp.cpp` and `hw_renderhacks.cpp`: BSP and
  render-hack reads/writes of `validcount` and world visitation fields.
