# Audit: OpenGL framegraph execution boundary — 2026-09-20

## Status

This audit covers the existing OpenGL postprocess and render-buffer paths and
defines the first OpenGL backend-use observation tranche. The CPU framegraph
already records the same named postprocess passes for GL and Vulkan; Vulkan and
now the OpenGL postprocess path validate declared uses against their backend
binding sites.

The implemented tranche is an order-preserving observation/validation layer
shared with Vulkan. It does not reorder GL draws, add speculative barriers, or
infer resource ownership from raw GL object bindings.

## Current execution model

`GLPPRenderState::Draw()` in `src/common/rendering/gl/gl_renderbuffers.cpp` is
immediate:

1. It records a named `PassDesc` before any GL bindings, when every input and
   output resolves to a stable registry name.
2. It saves the current texture state.
3. It binds input textures through `FGLRenderBuffers` or the named `PPTexture`
   backend object.
4. It binds the output framebuffer, configures blend/viewport/shader state,
   and draws the screen quad.
5. It advances the pipeline ping-pong index when the output is
   `NextPipelineTexture`.

The graph is therefore an observation of already-issued GL work, just as it is
for Vulkan. A later topological order cannot reschedule these draws.

## Findings

### Finding 1 — OpenGL records pass topology and now observes backend uses

Before this tranche, `GLPPRenderState::Draw()` added `reads` and `writes` but
did not populate `PassDesc::uses`, call `BeginBackendPass()`, or call
`ObserveBackendUse()`. The first integration tranche now adds those
declarations and hooks. The resource registry retains its independent
bind-time tracking through
`FGLRenderBuffers::Bind*()` and the direct named-`PPTexture` branches.

**Consequence:** the shared backend-neutral
`FrameGraphAccess`/`FrameGraphUsage` contract now validates the GL input/output
bind path without changing execution.

### Finding 2 — FBO attachment is not the same as a shader write

`BindSceneFB()` binds an FBO containing color and depth/stencil attachments and
currently touches both `SceneColor` and `SceneDepthStencil`. A postprocess draw
using `SceneColor` as its output has depth testing disabled by
`FGLPostProcessState`, however, so its graph-level write is the color
attachment only. Vulkan already follows this semantic distinction in
`VkRenderBuffers::GetOutput()`.

**Consequence:** GL graph observation must happen at the postprocess semantic
input/output boundary, or explicitly classify the color write separately. It
must not blindly convert every `Resources().Touch(..., true)` in an FBO bind
helper into a graph write.

### Finding 3 — The helper bind points are shared by scene rendering

These helpers are used both inside and outside postprocess passes:

- `BindSceneFB(bool sceneData)` — scene and scene-data rendering;
- `BindScene*Texture()` — postprocess inputs;
- `BindCurrentTexture()` — postprocess inputs and final presentation;
- `BindCurrentFB()` — postprocess outputs, 2D drawing, and other renderer work;
- `BindNextFB()` — postprocess outputs.

An observation hook in a helper must therefore be active only while
`GLPPRenderState::Draw()` has bracketed a graphable pass. Otherwise scene and
presentation operations would be incorrectly attributed to the current
postprocess pass.

### Finding 4 — GL has no active framegraph barrier authority

The current GL path contains no calls to `glMemoryBarrier()` or
`glTextureBarrier()`. The postprocess chain normally ping-pongs between
different textures, so it does not rely on sampling and rendering to the same
texture in one pass. The loader exposes texture-barrier entry points, but GL
capability flags do not currently track them.

**Consequence:** the first GL framegraph tranche must not add barriers merely
because the graph knows about a dependency. If a future optimization creates
same-image feedback or image-store hazards, capability detection and a
separate hazard contract must be designed first.

### Finding 5 — Resource identity can alias even when names differ

`FGLRenderBuffers::CreateScene()` has four MSAA/scene-data branches. In the
non-MSAA branches, `SceneColor` is backed by the same GL texture as
`PipelineImage[0]`; in other branches it is a separate multisample texture or
renderbuffer. `SceneColor`, `SceneDepthStencil`, `SceneFog`, and
`SceneNormal` also exist only in the branches that create them.

**Consequence:** the graph may use names for the current validation phase, but
future allocation, aliasing, or hazard analysis must consult the resource
registry's physical handle and format/sample metadata. Names alone cannot
prove that two GL resources are distinct.

### Finding 6 — Most blits and presentation remain outside the named graph

`BlitSceneToTexture()` now contributes a `scene.resolve` transfer pass when
MSAA is active, resolving scene color into `PipelineImage[0]` before
postprocessing. Eye-texture blits, `BindOutputFB()`/backbuffer presentation,
shadow-map rendering, stereo presentation, screenshots, and wipes still
perform real resource work outside `GLPPRenderState::Draw()`.

The current graph intentionally treats the scene inputs and pipeline start as
external boundaries, and leaves shadow/custom/presentation work ungraphable.
That is acceptable for validation, but it must not be mistaken for complete
GL frame coverage.

## Implemented first integration boundary

1. In `GLPPRenderState::Draw()`, populate `PassDesc::uses` from the same
   resolved names used for `reads` and `writes`.
2. Bracket only graphable passes with `BeginBackendPass()` and
   `EndBackendPass()`.
3. Observe sampled reads in the input switch and color writes in the output
   switch, including named `PPTexture` objects.
4. Keep `FGLRenderBuffers`' existing resource touches, FBO setup, state save/
   restore, and ping-pong behavior unchanged.
5. Run the existing CPU self-test, GL CI smoke path, and a real enabled GL
   postprocess chain. The live graph must agree with `FrameResources` without
   requiring every resource to be touched in the first frame.

The first transfer boundary is now implemented in
`FGLRenderBuffers::BlitSceneToTexture()`: the MSAA resolve is recorded as
`scene.resolve` and its source/destination uses are observed around the real
`glBlitFramebuffer()` call.

This is the same order-preserving contract now used by Vulkan. It gives GL a
shared correctness check without claiming that OpenGL has Vulkan-style layout
transitions or a graph-owned scheduler.

The full build and diff hygiene check pass. A live GL run remains pending on
this machine because its current X11/Wayland display session is not accepting
new renderer windows; the Vulkan live path and the CPU graph self-test were
already verified for the shared contract.

## Deferred work

- Model the scene render and `BlitSceneToTexture()` as explicit producer/
  transfer operations.
- Decide whether shadow maps, eye textures, presentation, screenshots, wipes,
  and custom shader textures need stable registry names.
- Add GL capability tracking only if a future graph executor needs texture
  barriers or image-memory barriers.
- Defer pass reordering, culling, transient allocation, and aliasing until
  the backend-neutral executable-pass contract and physical-resource rules
  are established.
