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
for Vulkan. A later topological order cannot reschedule these draws. Custom
postprocess shaders use the same boundary: their pass writes the next pipeline
image, while mod-provided texture inputs are graph-only external resources.

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

### Finding 6 — Blits and presentation are explicit, but coverage is not complete

`BlitSceneToTexture()` now contributes a `scene.resolve` transfer pass when
MSAA is active, resolving scene color into `PipelineImage[0]` before
postprocessing. Shadow-map production now contributes a `shadowmap` color
attachment pass. The normal single-eye `CopyToBackbuffer()` path now
contributes a `present` pass writing the graph-only `Backbuffer` boundary; its
bounded screenshot variant contributes `backbuffer.copy` as a color-attachment
write. Stereo eye stores/loads and `PresentStereo()` are now also represented
as `stereo.store`, `stereo.load`, and `present.stereo` passes. Wipe start/end
captures are represented as `wipe.copy` transfer passes with graph-only
destination names, while the texture objects remain engine-owned. The default
framebuffer remains OS-owned and is not a registry entry.

The current graph intentionally treats the scene inputs and pipeline start as
produced by the conservative `scene.target` boundary. The armed screenshot
path now contributes a read-only `screenshot.readback` pass after `present`:
it reads the graph-only `Backbuffer` boundary as a transfer source around the
pre-swap `glReadPixels` call. The CPU pixel array is deliberately not a frame
resource. The legacy unarmed post-swap read remains outside the graph because
its contents are platform-dependent and are not a valid Linux capture source.

## Implemented first integration boundary

1. In `GLPPRenderState::Draw()`, populate `PassDesc::uses` from the same
   resolved names used for `reads` and `writes`.
2. Bracket only graphable passes with `BeginBackendPass()` and
   `EndBackendPass()`.
3. Observe sampled reads in the input switch and color writes in the output
   switch, including named `PPTexture` objects.
4. Keep `FGLRenderBuffers`' existing resource touches, FBO setup, state save/
   restore, and ping-pong behavior unchanged.
5. Record custom postprocess passes even when their mod-provided texture inputs
   are not frame-owned resources: give those inputs stable graph-only external
   names instead of dropping the whole pass.
6. Run the existing CPU self-test, GL CI smoke path, and a real enabled GL
   postprocess chain. The live graph must agree with `FrameResources` without
   requiring every resource to be touched in the first frame.

The first transfer boundary is now implemented in
`FGLRenderBuffers::BlitSceneToTexture()`: the MSAA resolve is recorded as
`scene.resolve` and its source/destination uses are observed around the real
`glBlitFramebuffer()` call.

The normal single-eye presentation boundary is recorded by
`FGLRenderer::CopyToBackbuffer()`: the current pipeline image is observed as a
sampled read and the OS-owned default framebuffer is observed as a `Present`
write under the graph-only name `Backbuffer`. Screenshot readback through the
same helper is classified as a color-attachment write instead. The armed
pre-swap readback is then observed as a transfer-source read of `Backbuffer`.

The scene target boundary is recorded by
`OpenGLFrameBuffer::SetSceneRenderTarget()`: it declares the scene color,
depth/stencil, and optional SSAO G-buffer attachments as one conservative
producer. In the non-MSAA layout it also declares the graph alias between
`SceneColor` and `PipelineImage[0]`, so the postprocess dependency chain follows
the physical GL object rather than treating the two names as unrelated.

Named `PPTexture` inputs now observe their sampled use at the GL bind point,
matching Vulkan's existing descriptor observation.

This is the same order-preserving contract now used by Vulkan. It gives GL a
shared correctness check without claiming that OpenGL has Vulkan-style layout
transitions or a graph-owned scheduler.

The full build and diff hygiene checks pass. A real enabled postprocess run on
the RX 550 verified the GL graph at 42 passes and 45 edges; all active scene
and postprocess resources had matching write/read observations, with only the
unused pipeline depth buffer reported untouched. The same run completed
without stale-size or graph-build errors. The CPU self-test also covers the
alias normalization path.

## Deferred work

- Extend the conservative scene target boundary to cover full scene draw
  grouping once the graph can represent nested/deferred passes. The MSAA
  resolve boundary is already covered.
- The legacy unarmed post-swap screenshot read remains outside the graph because
  the default framebuffer contents are not guaranteed after `Swap()`. ShadowMap,
  custom shader inputs, and the two GL stereo eye textures now have stable graph
  names and observed producers/consumers; wipe destinations have graph-only names because
  their texture-object lifetime is outside the registry.
- Add GL capability tracking only if a future graph executor needs texture
  barriers or image-memory barriers.
- Defer pass reordering, culling, transient allocation, and aliasing until
  the backend-neutral executable-pass contract and physical-resource rules
  are established.
