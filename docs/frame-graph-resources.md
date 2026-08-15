# Resource registry — design sketch

Step 1 of the migration order in `docs/frame-analysis.md` §4: **declare the
resources, keep the existing execution.** No behaviour change, no new allocator, no
graph yet — a description of what exists, maintained at the sites that already
create these textures.

Written 2026-08-16. This is a sketch for review, not committed code. The point of
sketching before implementing is that the interface is cheap to argue about now and
expensive to change once forty call sites use it.

---

## Why this first, and what it buys before any graph exists

`frame-analysis.md` §3.3 found that resource lifetimes are managed per-effect by
hand, so nothing knows the *set* of live resources at a given frame size. Three
things follow immediately from fixing only that, with no graph:

1. **A memory answer.** The reference machine has a 1.5GB integrated GPU and the
   renderer currently cannot say how much of it the frame's targets occupy. The AO
   module alone holds nine textures that no shared code can see.
2. **Size-mismatch validation at declaration time.** AO's quarter-res buffer rounds
   scene heights 773 and 776 to the same 194 rows while the full-res consumers do
   not — the kind of relationship nobody can check today because nobody holds both
   numbers at once.
3. **A proof-of-execution instrument.** Per `renderer-methodology.md` §4, the most
   expensive failure mode here is an experiment that never ran. "Which resources
   were touched this frame" answers *which path executed* structurally, rather than
   depending on a log label existing on that path — which is exactly what the
   compute AO path lacks.

Those are worth having on their own. If the frame graph never happens, this is still
the diagnostic the last two sessions kept wishing for.

## Shape

Backend-neutral description, backend-owned memory. The registry never allocates in
phase 1 — it records what the backend already made, keyed by a stable name.

```cpp
// common/rendering/hwrenderer/frame/hw_resources.h

enum class ResourceFormat : uint8_t { Unknown, RGBA8, RGBA16F, R8, RG16F, D24S8, /*…*/ };

// How the size is DERIVED, not just what it is. This is the part that makes
// mismatches checkable: two resources that both claim SceneScaled(4) must agree,
// and a consumer expecting SceneFull must not be handed a SceneScaled(2).
struct SizeRule
{
    enum Kind : uint8_t { Fixed, SceneFull, SceneScaled, MipOf } kind = Fixed;
    int divisor = 1;        // SceneScaled
    int level = 0;          // MipOf
    const char *parent = nullptr;   // MipOf
};

struct ResourceDesc
{
    const char *name;       // stable, e.g. "SceneColor", "AO.Ambient0"
    const char *owner;      // "MtRenderBuffers", "MtAOModule", "PPBloom"
    int width = 0, height = 0;
    int samples = 1;
    ResourceFormat format = ResourceFormat::Unknown;
    SizeRule size;
    bool transient = false; // dead at end of frame — the future aliasing candidate
};

class FrameResources
{
public:
    // Called where the texture is created today. Re-declaring the same name with
    // different dimensions is the recreate path and is expected.
    void Declare(const ResourceDesc &desc, void *backendHandle);
    void Forget(const char *name);

    // Called where the resource is bound/written. Cheap: a frame counter store.
    void Touch(const char *name, bool asOutput);

    void BeginFrame(int sceneWidth, int sceneHeight);
    // Non-fatal by default: reports, does not throw. See "failure policy".
    void ValidateFrame(FString *report) const;

    void Dump(FString *out) const;      // r_resources
    size_t TotalBytes() const;
};
```

`backendHandle` is opaque — `MTL::Texture*`, `VkImage`, a GL name. The registry
never dereferences it; it exists so the dump can correlate with a capture, and so
phase 3 has somewhere to put ownership when it takes it.

## Registration, at sites that already exist

The whole cost is one line per creation. `MtAOModule::EnsureTextures`
(`mt_ao.cpp:1378`) already early-outs on a size comparison, so declaration lands
naturally next to it:

```cpp
void MtAOModule::EnsureTextures(int width, int height) {
    if (mAOTexture && mBlurTexture && mAOWidth == width && mAOHeight == height)
        return;
    // … existing creation …
    fb->Resources().Declare({ "AO.Ambient", "MtAOModule", width, height, 1,
                              ResourceFormat::RG16F, { SizeRule::SceneScaled, aoScale },
                              /*transient*/ true }, mAOTexture);
}
```

Declare **all three tiers** from `frame-analysis.md` §1 — including the private
compute-module textures, which are the ones with no visibility today and therefore
the ones the registry is most worth having for.

## What it reports

```
] r_resources
scene 1440x776   backend Metal   12 resources   58.4 MB

  name                owner            size        format   rule            frame  MB
  SceneColor          MtRenderBuffers  1440x776    RGBA16F  SceneFull        w r   8.5
  SceneDepthStencil   MtRenderBuffers  1440x776    D24S8    SceneFull        w r   4.3
  SceneNormal         MtRenderBuffers  1440x776    RGBA8    SceneFull        w r   4.3
  SceneFog            MtRenderBuffers  1440x776    R8       SceneFull        w r   1.1
  PipelineImage[0]    MtRenderBuffers  1440x776    RGBA16F  SceneFull        w r   8.5
  PipelineImage[1]    MtRenderBuffers  1440x776    RGBA16F  SceneFull        w r   8.5
  AO.Ambient          MtAOModule       360x194     RG16F    SceneScaled/4    w r   0.5
  AO.FullresResult    MtAOModule       1440x776    R8       SceneFull        w r   1.1
  AO.DepthPyramid     MtAOModule       720x388     R32F     SceneScaled/2    -     1.1
  …
  UNTOUCHED this frame: AO.DepthPyramid, Bloom.CompositeTex
```

The last line is the interesting one. "Declared but never touched" is how you notice
a path that did not run, a resource kept alive for a disabled feature, or an
algorithm variant nobody selected — the compute-AO questions from this session,
answered structurally.

## What it validates

Cheap, per frame, and each one corresponds to a bug that has actually occurred:

- **Size-rule agreement.** A `SceneScaled(4)` producer feeding a `SceneFull`
  consumer is either intentional upsampling or the AO 773/776 class of mismatch;
  the registry cannot tell which, but it can *show* it, which is more than exists now.
- **Stale size.** A resource whose recorded dimensions no longer match its rule
  applied to the current scene size — i.e. someone's `EnsureTextures` did not run.
- **Format surprises.** A consumer declaring it reads RGBA16F, bound to an R8.
- **Orphans.** Declared, never touched, for N frames.

**Failure policy: report, never abort.** These run in a shipping renderer, and a
false positive that kills the frame is worse than the bug it catches. Gate the
report behind a cvar (`r_resource_validate`), print to stderr per
`renderer-methodology.md` §4 so a harness can read it, and make CI assert the report
is empty on the smoke-test scene.

## Phases

| phase | registry does | risk |
|---|---|---|
| 1 | records and reports; backends still allocate | none — pure observation |
| 2 | validates rules, CI asserts empty report | false positives; mitigated by report-only |
| 3 | owns allocation; aliases non-overlapping transients | real; needs the graph's lifetimes |

Phase 3 is where the memory win is, and it is deliberately last: aliasing without
declared pass lifetimes is guesswork, and the lifetimes come from the graph.

## Open questions for review

1. **Names: strings or interned ids?** Strings are debuggable and match the dump;
   interning is faster for `Touch` on a hot path. My inclination is `const char*`
   with pointer-equality fast path, since every name is a literal.
2. **Where does it live?** `hwrenderer/` makes it shared with GL and Vulkan, which
   is the point — but Metal is the only backend that will register anything at
   first, and a shared header with one user is a smell. Acceptable if the Vulkan/GL
   analysis (the Linux task) follows soon; otherwise put it under `metal/` and lift
   it when the second backend arrives.
3. **Is `Touch` worth its cost?** It is a store per bind. If it turns out measurable
   on the reference machine — which it should not, against ~50 draws — gate it with
   the validation cvar rather than removing it, since "untouched this frame" is the
   most valuable line in the dump.
4. **Should `MtRenderBuffers` be first, or the AO module?** Render buffers are
   simpler; the AO module is where the visibility gap actually is. I would do
   `MtRenderBuffers` first to shake out the interface on easy resources, then AO
   immediately after, and not bother with the rest until the graph needs them.
