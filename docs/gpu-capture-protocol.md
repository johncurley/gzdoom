# GPU frame capture protocol — Metal

A runbook for taking a Metal GPU frame capture with `mt_capture` and reading a
specific answer out of it. Written for the AO composite investigation
(handoff.txt standing item 1), but the mechanics apply to any Metal pass.

This is a *different instrument* from the screenshot A/B protocol in
`handoff.txt`. A screenshot is the final pixels. A capture is the whole command
stream: every encoder, the pipeline state actually bound, the blend and stencil
configuration in force, attachment load/store actions, and every input and
output texture at full precision. Reach for it when the question is "what did
the GPU actually do", not "what did the frame look like".

Read `CLAUDE.md` → "Measuring a rendering change" first. The rule that a numeric
prediction is stated *before* looking applies here with more force than usual,
because a capture shows so much that it is easy to find a story in it after the
fact.

---

## 0. Pre-flight

**Clear the PSO binary archive.** Every pipeline is built through
`setBinaryArchives` (`mt_pipelinestate.cpp:221-226`), so a stale archive can
serve a pipeline that no longer matches the source. Capturing one of those looks
exactly like a genuine finding — you would be reading a pipeline the current
code never built.

```bash
rm -f ~/Library/Application\ Support/zdoom/cache/mt_pipelines.bin
```

**Check `gzdoom.ini`.** Non-default CVARs from earlier experiments produce
artifacts indistinguishable from broken code; an entire AO investigation on this
branch resolved to config. Confirm the settings you care about rather than
assuming, and note anything unusual in the capture log.

**Know what you are asking before you launch.** A capture at this viewpoint
costs well over a minute of slow frames, and the trace is large. Go in with the
specific list of values you intend to read (§4), not with "have a look around".

---

## 1. Launch

One launch per configuration, cvars on the command line, as in the screenshot
protocol. The operator types one word in the console.

```bash
METAL_CAPTURE_ENABLED=1 ./build/gzdoom.app/Contents/MacOS/gzdoom \
  -iwad DOOM2.wad \
  -file ~/Documents/GZDoom/Ashes2063Enriched2_23.pk3 \
  -loadgame capspot.zds \
  +gl_exposure_speed 1 +gl_ssao 3 \
  > /tmp/capture.log 2>&1
```

`METAL_CAPTURE_ENABLED=1` is **not optional**. macOS gates programmatic capture
behind an opt-in. The bundle's `Info.plist` carries `MetalCaptureEnabled`, but
that only applies when launched as a bundle, and this protocol runs the
executable directly. Without the variable, `mt_capture` refuses at arming time
and prints instructions rather than failing a minute later and wasting the
launch.

Keep the redirect. The log is how you prove which configuration a trace belongs
to — the same reason the screenshot protocol insists on it.

---

## 2. Capture

In the console:

```
mt_capture
```

Then **close the console**. The world does not render while it is open, so the
countdown does not advance and the capture never fires. This is the same
constraint `mt_bloom_dump` has, and the same one behind the screenshot
protocol's rule 4.

Default is 30 rendered frames. `mt_capture 60` if you want more settling — worth
it if exposure adaptation matters to the values you are reading.

The trace lands in `~/Documents/GZDoom/gputrace/frame-<timestamp>.gputrace`.
The console prints the exact path and an `open` command. Timestamped because
Metal refuses to overwrite an existing trace, so several captures per session
work without bookkeeping.

```bash
open ~/Documents/GZDoom/gputrace/frame-<timestamp>.gputrace
```

Opening it launches Xcode. No Xcode *build* is needed — only Xcode as a viewer.

---

## 3. Finding the pass

Postprocess passes label their render command encoder, so Xcode's frame
navigator lists them by name:

```
PP ssao: shaders/pp/ssaocombine.fp
PP bloom: shaders/pp/bloomcombine.fp
PP tonemap: shaders/pp/tonemap.fp
```

The name is `PP <group>: <fragment shader lump>`, built in
`MtPPRenderState::Draw`. If a pass shows as unlabeled, it did not come through
`MtPPRenderState` — the native compute AO and bloom paths have their own
pipelines and will not carry these labels.

**Confirm you are on the right frame before reading anything.** The trace holds
one frame. Verify the scene matches `capspot.zds` in the attachment preview. A
capture from a different viewpoint compares fine on every number and means
nothing — the trace equivalent of the "title bar is the control" rule, and worse
here because a trace has no HUD or title bar to catch it.

---

## 4. The four readings — AO composite

Fill the prediction column **before** opening the trace. This is the step that
caught every real defect on this branch.

| # | Reading | Where | Prediction | Actual |
|---|---------|-------|-----------|--------|
| 1 | Blending enabled? Factors? | Bound pipeline state → colour attachment 0 | enabled, `SourceAlpha` / `OneMinusSourceAlpha` | |
| 2 | Stencil test result — did the draw survive? | Encoder depth/stencil state; check fragments-in vs fragments-out | passes | |
| 3 | SceneColor attachment load action | Render pass descriptor → colour attachment 0 | `Load` | |
| 4 | `SceneFog.rgb` vs `SceneColor.rgb` at a **dark corner** pixel | Bound fragment textures; sample the same pixel in both | **not equal** | |

Predictions above are derived from source in `handoff.txt` item 1. Note #4
predicts *not equal*, which would mean the dead `ssaocombine` patch is **not**
the whole story and item 1 stays open — the outcome that costs more work, which
is why it is written down in advance.

### Where each one lives in Xcode

Written against **Xcode 14.2 on macOS 12.7.6**, the reference machine's version.
Later Xcode versions move this UI around; the structure below holds but the
exact panel names may not.

Opening the `.gputrace` loads the frame debugger directly — no project, no
scheme, no Xcode build. The **left navigator** is the command stream: command
buffers → render command encoders → draws. The encoder labels added in
`518af860b` show here, which is how you find the pass at all.

Selecting the *encoder* and selecting a *draw inside it* show different things,
and this investigation needs both.

- **Reading 1 (blend)** — select the **draw**. Its bound resources include the
  render pipeline state; open that for the pipeline descriptor, where colour
  attachment 0 carries `blendingEnabled` and the source/destination RGB and
  alpha factors. Take this reading first: it is the one that confirms or
  destroys the static audit.
- **Reading 3 (load action)** — select the **encoder**. That shows the render
  pass descriptor, with each colour attachment's texture, load action and store
  action.
- **Reading 4 (texture values)** — in the draw's bound resources, input textures
  are listed by index; double-click to open the texture viewer, which gives a
  per-pixel value readout. Zoom well in, pick a dark concave corner, and **write
  the pixel coordinates down** — you need the same pixel in both textures, and
  zoom and cursor position are easy to lose between them.
- **Reading 2 (did the draw survive)** — **do not** plan on fragments-in vs
  fragments-out. That is GPU counter data, and older Intel GPUs lack the
  stage-boundary counter sampling it needs (see `CLAUDE.md`); on the HD 6000 the
  counters may not exist. Use the **attachment contents** instead: the frame
  debugger shows the render target as of the selected draw, so compare SceneColor
  immediately before the `ssaocombine` draw with immediately after. Identical
  means the draw contributed nothing.

  This is the better instrument regardless of hardware. It observes the defect
  directly rather than through a proxy, and it stays valid whatever the cause —
  stencil, blend, or zero alpha. If you want the stencil state specifically, the
  depth/stencil state and stencil reference value appear on the encoder
  selection alongside the render pass descriptor.

### What each outcome means

- **#1 comes back disabled or with wrong factors** → the running binary does not
  match the source that was audited on 2026-08-06. That invalidates the premise
  of the whole static audit, including item 2. Suspect a stale archive (§0) or
  stale generated MSL before suspecting the code. Highest-information outcome.
- **#2 comes back failing** → a stencil test discarding the draw everywhere
  explains byte-identical frames completely and cleanly. This is the only piece
  of state on the path never checked at runtime. Second-highest information.
- **#3 comes back `Clear`** → the composite is blending into a cleared target.
  Check `mt_renderstate.cpp:1446-1449`, where `clear` is computed as
  `(mClearTargets & CT_Color) || !filled` — a SceneColor missing from
  `mClearedTargets` takes the clear path.
- **#4 comes back equal** → blending `fogColor` over an identical destination is
  a no-op, and the dead patch alone explains byte identity. Item 1 and item 2
  are one bug, and fixing the patch fixes both.
- **#4 comes back not equal** (predicted) → something else is zeroing the
  contribution. The dead patch is still a real defect and still worth fixing,
  but it is not the root cause. Do not close item 1 on it.

---

## 5. Ordering constraint

**Capture before fixing handoff item 2.** The `ssaocombine` patch rewrites
exactly the line producing this pass's colour and alpha. Once it lands, the
`gl_ssao 0 == gl_ssao 3` byte-identity is gone permanently, and if AO then
starts working you learn *that* something changed without learning *which*
candidate did it. Capturing first costs one launch and forecloses nothing.

---

## 6. Failure modes

| Symptom | Cause |
|---|---|
| "Metal capture is not enabled for this process" | `METAL_CAPTURE_ENABLED=1` missing. Refused at arming, so the launch is not wasted — set it and relaunch. |
| Armed, but nothing ever happens | Console left open. The world does not render, so the countdown never advances. |
| `mt_capture: the Metal backend is not active` | Running a non-Metal backend. Check `vid_preferbackend`. |
| Capture starts and never writes | The frame never completed. `MtCaptureEndFrameIfCapturing` fires at the end of `Update()`; a crash or early-out before it leaves the capture open. |
| Trace opens but passes are unlabeled | Not a `MtPPRenderState` pass — native compute AO/bloom have separate pipelines. |
| Pipeline state looks wrong in a way the source contradicts | Stale PSO archive. Clear it (§0) and recapture before drawing any conclusion. |

---

## 7. What to bring back

The four values from §4 with predictions alongside, plus the config line from
`/tmp/capture.log`. That is enough to complete the diagnosis; anything else from
the trace is context.

---

## Status

`mt_capture` was added 2026-08-06 (commit `117eb475f`) and the encoder labels
shortly after. It builds, the CCMD is registered, and the `Info.plist` key is in
the bundle, but **no capture had been taken at the time of writing** — the first
run exercises this path for the first time. If it misbehaves, suspect the arming
path first, and update this line once a capture has actually succeeded.
