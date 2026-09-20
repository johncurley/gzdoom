# Audit: Vulkan framegraph execution boundary — 2026-09-20

## Status

This is the first Vulkan-first framegraph audit tranche. It records the
current execution boundary and adds an order-preserving backend-use contract
to validate Vulkan's existing binding path. No renderer behavior is changed:
there is still no graph scheduling, barrier emission, or resource aliasing.

The working tree is `metal-audit`. The existing framegraph implementation is
the CPU-only recorder in `src/common/rendering/hwrenderer/frame/`; the Vulkan
backend is the first execution target for this tranche. Metal, allocation
aliasing, and scheduling optimization are out of scope here.

The resource-use metadata is backend-neutral by design. OpenGL can adopt the
same contract later at its texture/framebuffer bind points; this tranche wires
only Vulkan so the two renderer audits remain independently verifiable.

## Implemented in this tranche

- `FrameGraphAccess` and `FrameGraphUsage` describe sampled reads and color
  attachment writes without exposing Vulkan enums.
- Vulkan postprocess passes declare those uses and bracket their existing
  descriptor/framebuffer binding sites with observation hooks. The MSAA and
  non-MSAA scene-to-pipeline transfer is also recorded as `scene.resolve`,
  using `TransferSource`/`TransferDestination` around the existing resolve or
  blit operation.
- `Build()` reports declaration/observation mismatches, and the self-test
  covers sampled, color-attachment, depth-attachment, transfer,
  read/write-storage, and presentation uses, plus a deliberately invalid
  declaration.
- An earlier real Vulkan MAP01 run exercised the full enabled postprocess
  chain: 40 passes, 38 edges, and matching declared/observed uses. A current
  smoke run with the scene-resolve boundary initialized the RX 550 Vulkan
  backend and reached `+MAP01 - Entryway`; the command was intentionally
  timeout-bounded rather than used as a framegraph dump.

## Current execution model

`VkPostprocess::BlitSceneToPostprocess()` and `VkPPRenderState::Draw()` are
immediate. The former records the scene resolve/blit boundary before the latter
records the postprocess chain. `VkPPRenderState::Draw()` ends the active Vulkan
render pass, records the graph description, resolves the input descriptors,
resolves the output framebuffer, emits the draw, and advances the ping-pong
image. The relevant sequence is in `vulkan/renderer/vk_pprenderstate.cpp`:

1. `EndRenderPass()` closes any scene pass.
2. `GetInput()` transitions sampled images to
   `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.
3. `GetOutput()` transitions the output to
   `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` and attaches the required depth
   image.
4. `RenderScreenQuad()` begins and ends the postprocess render pass.
5. The current pipeline image is advanced if the output is `NextPipelineTexture`.

The current graph therefore observes the legal order in which work has already
been encoded. `PassDesc` contains only a name, owner, resource names, and RAW
dependencies. It does not contain the shader, uniforms, descriptor choices,
viewport, blend state, framebuffer, or an executable callback.

## Findings

### Finding 1 — sorting the current graph cannot schedule Vulkan work

**Location:** `src/common/rendering/vulkan/renderer/vk_pprenderstate.cpp:64-137`,
`src/common/rendering/hwrenderer/frame/hw_framegraph.h:44-50`.

`AddPass()` is called immediately before command encoding. A later topological
sort can report an order, but it cannot change commands already written into
the command buffer. A scheduler would need either deferred pass capture or a
separate planning phase that runs before any pass emits commands.

**Consequence:** the first Vulkan implementation must not claim to reorder or
execute the graph. The safe first step is an order-preserving plan that
validates uses and supplies conservative transition information to the existing
draw path.

### Finding 2 — image state and barriers are not graph-owned

**Location:**
`src/common/rendering/vulkan/renderer/vk_descriptorset.cpp:195-228`,
`src/common/rendering/vulkan/textures/vk_renderbuffers.cpp:282-306`,
`src/common/rendering/vulkan/textures/vk_imagetransition.cpp:25-104`.

Vulkan transitions are issued opportunistically at descriptor and framebuffer
binding sites. `VkImageTransition` reads and mutates `VkTextureImage::Layout`
while constructing the barrier. Other transitions are emitted by scene blits,
shadow-map updates, active-target setup, and presentation helpers.

**Consequence:** a graph-generated barrier plan must not coexist with an
independent second layout authority. Until ownership is deliberately moved,
the graph may describe required uses and validate the existing transitions,
but it must not emit duplicate barriers.

### Finding 3 — resource names are insufficient Vulkan execution metadata

The graph currently knows only that a pass reads or writes a named resource.
Vulkan execution also needs the usage class and access scope: sampled image,
color attachment, depth/stencil attachment, transfer source/destination, or
presentation; plus the image subresource range and the relevant pipeline stage
and access masks.

**Consequence:** the next graph contract needs an abstract resource-use layer,
separate from Vulkan enums, before it can derive barriers. It must also preserve
the existing resource registry's physical identity, format, sample count, and
size information.

### Finding 4 — pass coverage is intentionally incomplete

The current graph records the named postprocess chain and the named
bloom/exposure/AO resources. It does not yet model all Vulkan work:

- swapchain/presentation output;
- shadow-map rendering;
- custom shader inputs whose textures have no stable registry name;
- scene rendering as a multi-attachment producer;
- screenshot, wipe, stereo, and other presentation variants.

These are coverage gaps, not reasons to invent placeholder names. An unresolved
resource must remain explicitly external or ungraphable until its ownership and
usage are defined.

### Finding 5 — the Vulkan render-pass cache is part of the contract

`VkRenderPassManager::RenderBuffersReset()` clears cached scene and postprocess
render-pass setups when render-buffer dimensions or sample count change. Any
future graph-owned resource lifetime or framebuffer plan must invalidate or
key this cache consistently. A graph resource name alone is not a sufficient
render-pass identity.

## Agreed first implementation boundary

The first Vulkan tranche will be correctness-first and order-preserving:

1. Define backend-neutral resource-use metadata.
2. Extend graph validation to check that each pass's declared use is
   compatible with the resource declaration and the known pass order.
3. Add a conservative Vulkan-side plan or validation hook that observes the
   existing transition sites; it must not emit a second barrier system.
4. Keep the existing immediate draw path and render-pass cache unchanged until
   the plan has live evidence.
5. Defer pass reordering, pass culling, transient allocation, aliasing, and
   memory optimization.

The eventual scheduler will require a separate executable-pass contract. That
contract must capture or reference the complete state needed to replay a pass;
resource names and dependency edges alone are not enough.

## Verification required before executor work

- A CPU self-test for sampled-read, color-write, depth-write, transfer, and
  read/write hazard combinations. The self-test now covers this contract.
- A live Vulkan run proving the observed use sequence agrees with the existing
  `VkImageTransition` calls.
- Resize and MSAA/sample-count changes, because they reset both resources and
  Vulkan render-pass caches.
- A control run with the graph hook disabled, so a passing validator proves it
  can detect a deliberately mismatched use rather than merely observing the
  normal path.
- The same shared-code build under Windows Visual Studio when available.

No Metal code is required for this tranche. Metal integration and any
Apple-Silicon-specific optimization remain a later consolidation step.
