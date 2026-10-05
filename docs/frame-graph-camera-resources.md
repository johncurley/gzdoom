# Frame-graph camera target contract

Status: implemented for GL, Vulkan, and Metal source on 2026-10-03. The Linux
build, graph self-test, and live GL/Vulkan camera fixtures pass. Intel Metal
rebuild and live fixture validation passed on 2026-10-05; Apple Silicon/TBDR
policy is outside this change.

## Scope

Camera textures are rendered synchronously inside `RenderTextureView()`. The
backend binds a camera color texture and a depth/stencil attachment, then the
shared `RenderViewpoint()` path clears and draws the camera view. Today the
graph records one keep-alive `offscreen.camera` color producer around that
callback. It does not distinguish the target clear, opaque scene, and
portal/translucent scene work.

This contract gives those operations graph identities. The graph remains a
diagnostic description of existing backend work. It does not create GPU render
passes, change command submission, or authorize reordering, culling, barriers,
or resource aliasing.

## Resource identities

The camera color resource is the existing hardware-texture name returned by
`GetFrameGraphResourceName()` (`GL.Texture.N`, `Vulkan.Texture.N`, or
`Metal.Texture.N`). It is already used when camera textures are sampled by
materials. Keep this exact name so the camera producer connects to later
offscreen or main-view sampled reads.

The depth/stencil attachment is a separate resource whose stable name is
derived from that same target identity, for example
`<color-resource>.DepthStencil`. This matches the current ownership:

| Backend | Current camera depth/stencil storage |
|---|---|
| OpenGL | A renderbuffer held by the target's `FHardwareTexture` wrapper and attached for depth and stencil. |
| Vulkan | A depth/stencil image held by the target's `VkHardwareTexture` wrapper. |
| Metal | A depth/stencil texture held by `MtTextureManager` for the `FCanvasTexture`; it may use `StorageModeMemoryless`. |

For `FCanvasTexture` camera targets, all three backends bind a depth/stencil
attachment. The graph must describe the attachment actually bound for this
target, not infer it from the main view's `SceneDepthStencil` name.

Camera targets have no `SceneFog` or `SceneNormal` attachments and do not use
the main scene's multisample/resolve pair. Do not add those resources to camera
passes based on `FRenderState::GetPassType()`; that value does not define the
camera target's physical attachment set. The active render-target context is
the source of truth.

These names are graph identities, not a new allocation policy. Do not classify
camera depth as transient or use it in alias/lifetime decisions through this
contract. In particular, Metal's memoryless depth storage cannot be treated as
a value that survives an independently scheduled GPU pass boundary.

## Camera pass sequence

The camera callback exposes these diagnostic passes in authored execution
order:

1. `offscreen.camera.clear` writes the camera color and depth/stencil
   attachments. `Clear3DViewport()` clears both before scene drawing, so this
   pass does not read prior attachment contents.
2. `offscreen.camera.opaque` logically reads and writes camera color and
   depth/stencil. The reads represent attachment preservation, blending, and
   depth testing. Backend observations remain writes using color-attachment
   and depth/stencil-attachment usages. Material texture reads made during the
   opaque traversal attach to this pass.
3. `offscreen.camera.portal_translucent` logically reads and writes the same
   attachments and records sampled material reads during portal and
   translucent drawing. It is the keep-alive root for the completed camera
   update; RAW attachment dependencies retain the clear and opaque producers.

The current aggregate camera producer must not remain as another writer of the
same color resource when these three passes are recorded. That would create a
duplicate producer and obscure which camera operation supplies each version.
Canvas updates remain on their existing aggregate `offscreen.canvas` path:
they are 2D updates that preserve prior target pixels and are not camera scene
traversals.

The callback may update more than one camera target in a frame. Each update
uses its own color and derived depth/stencil resource identities. Pass names
may repeat; each pass's resource lists identify which target it describes.

## Scope and backend constraints

The active target context is scoped to one `RenderTextureView()` callback and
must be restored with the backend render target when the callback ends. Scene
pass recording and clear recording use that context. They must not accidentally
fall back to main-view `Scene*` names while a camera target is bound.

The target context does not itself mean a graph pass is active for the whole
callback. The clear, opaque, and portal/translucent graph scopes begin around
their corresponding shared operations so `ObserveSceneMaterialRead()` attaches
each sampled input to the pass that consumed it. A sampled camera texture with
no producer in the current frame is a valid existing value; retain the current
external-input behavior for that case.

All graph scopes describe logical dependencies only. Metal may split existing
draw submission into encoders internally, and its depth attachment may be
memoryless. This contract does not claim that camera subpasses can be encoded
as separate Metal render passes while preserving attachment contents. Any
future scheduler must first prove load/store and memoryless behavior on the
target hardware.

## Validation record

- The graph self-test covers clear → opaque → portal/translucent RAW
  dependencies for both target attachments, sampled material reads in both
  scene phases, distinct stable identities for multiple targets, and exact
  target writes that reject accidental main-view `Scene*` use.
- The existing canvas aggregate and preserved-content read remain covered by
  the self-test.
- Live GL camera fixture: 52 passes / 114 edges, with all three camera phases,
  target color and depth/stencil, material reads, `Backbuffer` reachability,
  and no dead-pass candidates or graph diagnostics.
- Live Vulkan camera fixture: 12 passes / 24 edges with the same camera
  attachment chain and reachability. `VK_LAYER_KHRONOS_validation` reported no
  errors.
- Intel Metal's precached frame-1 fixture passed on 2026-10-05: 14 passes /
  32 edges, two distinct camera color/depth identities, all three phases,
  material reads in opaque and translucent phases, a rooted `Backbuffer`,
  and no dead-pass or resource-validation errors. Details are in
  [`handoff-macos-2026-10-04.md`](handoff-macos-2026-10-04.md).
  This does not claim Apple Silicon or TBDR policy coverage.
- No graph-on/off image or execution-order change is expected from recording
  these scopes. Any such difference is a regression to resolve before moving
  on to ordering experiments.
