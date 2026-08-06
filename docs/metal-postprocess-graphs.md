# Metal postprocess pass graphs

This is a pass-connection reference, not a shader reference.  A shader tells
us how one pass transforms its inputs; the C++ `Render()` method tells us which
textures are connected, in which order, and under which conditions.  That
second part is the contract most likely to produce a silent rendering defect.

Start at the Metal entry point:

```text
MetalRenderDevice::AmbientOccludeScene
  -> MtPostprocess::AmbientOccludeScene
       -> native compute AO, if selected and its resources/pipelines work
       -> PPAmbientOcclusion::Render, otherwise  [this document]
```

On the Intel development machine, `mt_compute_ao_intel` is false by default,
so Metal selects this reference raster path.  OpenGL and Vulkan use the same
`PPAmbientOcclusion::Render()` method through their own `PPRenderState`
implementations.  The graph is therefore a useful cross-backend specification;
the Metal-specific layer maps each `Draw()` to a labeled Metal render encoder.

## How to read a `Render()` pass sequence

`PPRenderState::Clear()` resets the description of the *next* pass.  The calls
following it establish that pass's shader, inputs, output, viewport, and blend
state; `Draw()` submits the full-screen triangle.  `PushGroup("ssao")` brackets
the sequence.  Metal records the group and shader name as an encoder label,
such as `PP ssao: shaders/pp/ssaocombine.fp`, which makes the same sequence
visible in an Xcode capture.

The AO intermediate textures are half-resolution:

```text
LinearDepthTexture : R32f     (linear view depth in .r)
Ambient0           : Rg16f   (.r = AO attenuation, .g = linear view depth)
Ambient1           : Rg16f   (same payload while ping-ponging the blur)
```

`1.0` in the AO channel means no darkening; values below it produce more
occlusion.  The depth channel is not decorative: the blur uses it to avoid
mixing across a depth edge and the composite uses it to decide whether it may
write any AO at all.

## Reference raster AO graph

Source: `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.cpp`,
`PPAmbientOcclusion::Render()`.

```text
SceneDepth.x + SceneColor.a
              |
              v
        [linear depth] ------------------------> LinearDepthTexture.r
              |
              v
LinearDepthTexture.r + SceneNormal.xyz + random texture
              |
              v
             [SSAO] --------------------------> Ambient0.(occlusion, viewZ)
                                                     |
                  +----------------------------------+
                  |  gl_ssao_debug < 2 only
                  v
        [depth blur, horizontal] --------------------> Ambient1.(occlusion, viewZ)
                  |
                  v
        [depth blur, vertical] ----------------------> Ambient0.(occlusion, viewZ)
                                                     |
SceneFog.rgb ----------------------------------------+
                                                     v
                                              [SSAO combine]
                                                     |
                                                     v
                                                SceneColor
```

| Pass | Reads | Writes | Pipeline-changing gates / state |
|---|---|---|---|
| `lineardepth` | `SceneDepth.x`, `SceneColor.a` | `LinearDepthTexture.r` | Chooses `LinearDepthMS` when `gl_multisample > 1`; otherwise the normal shader. No blend. |
| `ssao` | `LinearDepthTexture.r`, `SceneNormal.xyz`, quality-specific random texture | `Ambient0.r = attenuation`, `Ambient0.g = viewZ` | Chooses `AmbientOccludeMS` with MSAA. No blend. |
| `depthblur` horizontal | `Ambient0.(r,g)` | `Ambient1.(r,g)` | Runs only when `gl_ssao_debug < 2`. No blend. |
| `depthblur` vertical | `Ambient1.(r,g)` | `Ambient0.(r,g)` | Runs only when `gl_ssao_debug < 2`. No blend. It exponentiates the final attenuation and must preserve the centre depth in `.g`. |
| `ssaocombine` | normally `Ambient0.(r,g)` and `SceneFog.rgb` | `SceneColor` | `gl_ssao_debug == 4` instead supplies `SceneNormal` in input slot 0. Debug modes replace the target (no blend); normal mode alpha-blends the AO contribution over `SceneColor`. Metal also enables the scene stencil test for this `SceneColor` target. |

The table deliberately describes *payloads*, not merely texture names.  Names
like `Ambient0` say where data lives, but do not say which channel a later pass
is allowed to depend on.

## Step-by-step

1. **Linearize depth.** The depth buffer is non-linear and Metal may use
   reverse-Z. `lineardepth.fp` uses the supplied coefficients to turn it into
   linear view depth. It checks `SceneColor.a`: an alpha of zero is treated as
   empty/sky and substituted with the far-depth value. This writes only the
   red channel because later AO calculations need one scalar depth per pixel.

2. **Calculate raw SSAO.** `ssao.fp` reconstructs a view-space position from
   the linear depth, reads the scene normal, and samples a rotated pattern of
   neighbours. It writes the attenuation to `.r` and the centre pixel's linear
   view depth to `.g`. The result is deliberately at half resolution, making
   this the data that both blur passes exchange.

3. **Blur horizontally.** The bilateral blur reads both channels of
   `Ambient0`. Nearby samples with a very different `.g` get a low weight, so
   a wall edge should not smear its occlusion into the background. It writes
   blurred attenuation and the unmodified centre depth to `Ambient1`.

4. **Blur vertically.** The same operation reads `Ambient1` and returns the
   final result to `Ambient0`. This is the last writer of the texture consumed
   by the composite, so its `.g` write is a particularly important handoff.

5. **Composite into the scene.** `ssaocombine.fp` reads `Ambient0.r` as the
   attenuation and `Ambient0.g` as depth. In normal mode it emits `SceneFog`'s
   RGB with an alpha computed from both values; ordinary source-alpha blending
   applies that darkening to the existing `SceneColor`. The output target must
   retain its previous contents (`Load` in a Metal capture), because this pass
   modifies the scene rather than replacing it.

## Gates that can invalidate an observation

These controls change which passes or bindings exist, rather than merely
changing a displayed value. Treat a screenshot taken with one of them as a
test of a *different graph*.

| Gate | Effect on the graph |
|---|---|
| `gl_ssao == 0`, zero scene width/height | Returns before `PushGroup`; no AO passes are submitted. |
| `gl_ssao_strength <= 0 && gl_ssao_debug == 0` | Returns before any AO pass. Debug mode intentionally overrides this early-out. |
| `gl_ssao_debug >= 2` | **Skips both blur passes.** The composite reads raw `ssao` output from `Ambient0`, not the final blur output. This is the critical observation trap. |
| `gl_ssao_debug == 4` | Rebinds input 0 of `ssaocombine` from `Ambient0` to `SceneNormal`; it is a normal debug visualization, not a composite test. |
| `gl_ssao_debug != 0` | Disables alpha blending so the debug image replaces the target. Normal rendering alpha-blends. |
| `gl_multisample > 1` | Chooses MSAA variants for depth linearization, SSAO, and composite. The pass topology remains five rows. |
| `mt_compute_ao` selection | On Metal only, successful native compute AO bypasses this entire reference graph. On Intel it is normally gated off unless `mt_compute_ao_intel 1` forces it. |

The most consequential one is `gl_ssao_debug < 2`: a depth-oriented debug
view can show an intact `.g` channel precisely because it bypasses the blur
pass that would otherwise be responsible for damaging that channel.

## Unenforced handoff contracts

No type or API boundary enforces the following.  They are the first things to
write down and test when a pass looks correct in isolation but the final frame
does not change.

| Producer -> consumer | Required contract | Failure signature |
|---|---|---|
| `lineardepth` -> `ssao` | `LinearDepthTexture.r` is positive linear view depth; `SceneColor.a == 0` maps to the far plane. | Invalid reconstructed positions and incorrect AO radius/fade. |
| `ssao` -> both blur passes | `Ambient*.r` is attenuation and `Ambient*.g` is the centre linear depth. | Blur ignores geometry boundaries or mixes unrelated surfaces. |
| horizontal -> vertical blur | The horizontal pass preserves the centre `.g`; it must not write a filtered or unrelated depth value. | Direction-dependent edge bleeding. |
| vertical blur -> `ssaocombine` | `Ambient0.g` still contains a positive depth. `ssaocombine` requires `ssao.y > 2.0` before it produces nonzero normal-mode alpha. | AO is calculated but has no visible effect. |
| `ssaocombine` -> `SceneColor` | `SceneColor` is loaded, the normal path uses source-alpha blending, and the Metal stencil test admits the intended pixels. | Correct composite output is discarded, overwritten, or leaks through portals. |

The vertical-blur-to-composite contract was the AO defect caught by the probe:
when the vertical blur wrote `0.0` to `.g`, the probe still saw real raw AO
(`Ambient0.r` mean about `0.867`) but saw a maximum `.g` of `0.0` and zero
pixels passing the composite depth gate. The composite consequently generated
zero alpha and a before/after `SceneColor` comparison was identical. The
current vertical shader writes `centerDepth` to `.g`, which restores the
contract; the point of preserving this graph is that the reason is now
explicit rather than encoded only in a shader comment.

**Status: fixed in source, NOT yet verified at runtime.** The corrected shader
has never executed. The first verification attempt loaded a stale pk3 — the app
bundle carries its own `gzdoom.pk3` next to the executable and that is the one
loaded, while the documented rebuild command updated `build/gzdoom.pk3`, which
nothing reads (see `CLAUDE.md`). The probe therefore reported the defect
unchanged, which is indistinguishable from a wrong diagnosis. One `mt_ao_probe`
run closes this; until then the claim is "diagnosed and repaired in source".

**Design decision, 2026-08-06: the depth dependency is kept deliberately.**
`master` composites with `vec4(fogColor, 1.0 - attenutation)` and has no depth
requirement at all — `depthSignal`/`depthMask` arrived in `595ffaf0f` and the
`ssao.y > 2.0` gate in `ac0fec5db`, both branch-local. So preserving `.g` was
one of two valid repairs; the other was reverting to master's line and dropping
the depth dependency entirely. Preserving was chosen, which means AO carries a
near-field falloff: `depthMask = 1 - exp2(-y * 0.01)` is about 0.29 at 50 world
units, rising to ~0.9 by 400. Expect AO to be weaker close to the camera than on
`master` or on stock GZDoom. That is intended behaviour under this decision, not
a regression — record any future "AO looks weak up close" against this line.

## A repeatable way to graph the next sequence

For each `Render()` method, make one row per `Draw()` and fill it from the C++
calls immediately preceding that draw:

1. Read every `SetInput*` call as an input edge, including its binding index,
   filter, and special source (`SceneColor`, `SceneFog`, and so on).
2. Read `SetOutput*` as the output edge and record the target's format and
   any meaningful channel payload.
3. Record `SetNoBlend`, `SetAlphaBlend`, or additive blending as part of the
   output contract, not as incidental state.
4. Promote any conditional that skips a `Draw()` or changes an input/output
   binding to a graph gate.
5. Check the producer's channel writes against the consumer's channel reads.
   That final comparison is where the AO bug became obvious.

Bloom is the next suitable sequence, but it should be documented separately:
Metal may select its native compute implementation while the reference bloom
still uses this raster `PPRenderState` model.  Keeping each `Render()` chain
small and independently checked prevents a large, speculative frame graph from
masquerading as an architectural document.
