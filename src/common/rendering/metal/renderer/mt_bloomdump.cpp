// Stage-by-stage bloom pyramid dump, for comparing the Metal compute path
// against the reference PP path level for level.
//
// Why this exists: four source reads eliminated every static candidate for the
// measured compute-vs-reference bloom difference (level count, level-0
// resolution, composite weights, resample kernel, pyramid structure, tap
// placement, Gaussian exponent, amount floor, resample coordinates, the
// upscale-replace leg, blur weight normalization, and dispatch conformance).
// The final composite cannot say WHICH stage diverges, so this reports each
// level separately.
//
// The measured signature it exists to localize is directional, not just
// "broader": the compute blob is ~68% taller and ~20% narrower than the
// reference's, reproducibly, at two viewpoints. So the number to watch below
// is the per-level bbox aspect, and the question is: at which level does the
// vertical extent first exceed the reference's?
//
// Timing: the dump runs at the TOP of a frame, reading the textures the
// PREVIOUS frame left behind. That is deliberate. The compute pyramid is
// encoded into the render command buffer and has not executed yet at the point
// it is built, so reading it in the same frame would need a mid-frame stall.
// Last frame's textures are complete and idle, and with the capture protocol's
// savegame-pinned static viewpoint they hold the same image.
//
// Only one path's textures are fresh in any given run, because the capture
// protocol uses one launch per configuration. Run the dump once per config.

#include "mt_bloom.h"
#include "mt_postprocess.h"
#include "../system/mt_renderdevice.h"
#include "../textures/mt_texture.h"
#include "../system/mt_commandbuffer.h"
#include "mt_renderstate.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "c_dispatch.h"
#include "printf.h"
#include "filesystem.h"
#include "m_png.h"
#include "files.h"
#include "cmdlib.h"
#include "i_specialpaths.h"
#include <Metal/Metal.hpp>
#include <vector>
#include <cmath>

// mt_debug.cpp has an identical helper but it lives in an anonymous namespace,
// so it cannot be shared. Same two-line check, kept local.
static MetalRenderDevice *ActiveMetalDevice() {
  if (!screen || !screen->IsMetal())
    return nullptr;
  return static_cast<MetalRenderDevice *>(screen);
}

static bool gBloomDumpArmed = false;
// Frames to keep waiting for a pyramid that actually has content. Arming from
// the command line (+mt_bloom_dump) happens before the map has loaded, and the
// first frames have empty or unallocated levels, so a one-shot dump would
// always miss. Bounded so that arming it with bloom disabled gives up instead
// of retrying forever.
static int gBloomDumpFramesLeft = 0;

// True on the frames just before the dump, so MtBloomModule::Execute snapshots
// its extract output. Asked for a couple of frames early because the dump reads
// the PREVIOUS frame's textures.
// Persistent copy of the reference path's extract output, filled by the
// PPBloom::DebugAfterExtract hook.
static MTL::Texture *gRefExtractSnapshot = nullptr;
static MTL::Texture *gRefBlurHSnapshot = nullptr;
static MTL::Texture *gRefBlurVSnapshot = nullptr;
static MetalRenderDevice *gDumpDevice = nullptr;

bool MtBloomDumpWantsExtract() {
  return gBloomDumpArmed && gBloomDumpFramesLeft <= 3;
}

namespace {

struct LevelStats {
  int width = 0, height = 0;
  float maxLum = 0.0f;
  int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
  int count = 0;
  double energy = 0.0;
};

// Reads an RGBA16Float texture back to the CPU.
//
// Blits into a Shared buffer on its own command buffer and waits. Safe only
// because this runs at the top of a frame on textures the previous frame
// finished with -- see the file header.
static bool ReadTexture(MetalRenderDevice *fb, MTL::Texture *tex,
                        std::vector<float> &outRGB, int &w, int &h) {
  if (!tex)
    return false;
  // Handles both formats a bloom level can have, so the tool still works
  // either side of the Rgba16f mapping fix and can show the difference.
  const MTL::PixelFormat fmt = tex->pixelFormat();
  const bool halfFloat = fmt == MTL::PixelFormatRGBA16Float;
  const bool bgra8 = fmt == MTL::PixelFormatBGRA8Unorm;
  if (!halfFloat && !bgra8) {
    Printf(PRINT_HIGH, "  (skipped: unhandled pixel format %d)\n", (int)fmt);
    return false;
  }

  w = (int)tex->width();
  h = (int)tex->height();
  const size_t bytesPerRow = (size_t)w * (halfFloat ? 8 : 4);
  const size_t dataSize = bytesPerRow * (size_t)h;

  MTL::Buffer *staging =
      fb->device->device->newBuffer(dataSize, MTL::ResourceStorageModeShared);
  if (!staging)
    return false;

  auto cmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
  if (!cmdBuf) {
    staging->release();
    return false;
  }
  auto blit = cmdBuf->blitCommandEncoder();
  blit->copyFromTexture(tex, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w, h, 1),
                        staging, 0, bytesPerRow, dataSize);
  blit->endEncoding();
  cmdBuf->commit();
  cmdBuf->waitUntilCompleted();
  cmdBuf->release();

  outRGB.resize((size_t)w * h * 3);
  if (halfFloat) {
    const __fp16 *pixels = (const __fp16 *)staging->contents();
    for (int i = 0; i < w * h; i++) {
      outRGB[i * 3 + 0] = (float)pixels[i * 4 + 0];
      outRGB[i * 3 + 1] = (float)pixels[i * 4 + 1];
      outRGB[i * 3 + 2] = (float)pixels[i * 4 + 2];
    }
  } else {
    // BGRA8Unorm: B in byte 0, R in byte 2, normalized to 0..1 so the numbers
    // are directly comparable against a half-float level.
    const uint8_t *pixels = (const uint8_t *)staging->contents();
    for (int i = 0; i < w * h; i++) {
      outRGB[i * 3 + 0] = pixels[i * 4 + 2] / 255.0f;
      outRGB[i * 3 + 1] = pixels[i * 4 + 1] / 255.0f;
      outRGB[i * 3 + 2] = pixels[i * 4 + 0] / 255.0f;
    }
  }
  staging->release();
  return true;
}

// Extent is measured against a fraction of the level's OWN peak, not an
// absolute value. The levels differ in both resolution and magnitude, so a
// fixed threshold would conflate a gain difference with a shape difference --
// and shape is the open question.
static LevelStats ComputeStats(const std::vector<float> &rgb, int w, int h,
                               float relThreshold) {
  LevelStats s;
  s.width = w;
  s.height = h;
  std::vector<float> lum((size_t)w * h);
  for (int i = 0; i < w * h; i++) {
    float l = rgb[i * 3 + 0] * 0.299f + rgb[i * 3 + 1] * 0.587f +
              rgb[i * 3 + 2] * 0.114f;
    lum[i] = l;
    if (l > s.maxLum)
      s.maxLum = l;
  }
  const float cut = s.maxLum * relThreshold;
  s.x0 = w;
  s.y0 = h;
  s.x1 = -1;
  s.y1 = -1;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float l = lum[(size_t)y * w + x];
      if (l > cut && s.maxLum > 0.0f) {
        s.count++;
        s.energy += l;
        if (x < s.x0) s.x0 = x;
        if (x > s.x1) s.x1 = x;
        if (y < s.y0) s.y0 = y;
        if (y > s.y1) s.y1 = y;
      }
    }
  }
  return s;
}

static void WritePNG(const std::vector<float> &rgb, int w, int h, float maxLum,
                     const char *path) {
  if (maxLum <= 0.0f)
    return;
  // Normalized by the level's own peak so a dim coarse level is still legible.
  // The peak is printed alongside, so the absolute scale is recoverable.
  std::vector<uint8_t> bytes((size_t)w * h * 3);
  const float inv = 1.0f / maxLum;
  for (size_t i = 0; i < (size_t)w * h * 3; i++) {
    float v = rgb[i] * inv;
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    bytes[i] = (uint8_t)(v * 255.0f + 0.5f);
  }
  FileWriter *file = FileWriter::Open(path);
  if (!file) {
    Printf(PRINT_HIGH, "  (could not open %s for writing)\n", path);
    return;
  }
  if (M_CreatePNG(file, bytes.data(), nullptr, SS_RGB, w, h, w * 3, 1.0f))
    M_FinishPNG(file);
  delete file;
}

static void DumpOne(MetalRenderDevice *fb, MTL::Texture *tex, const char *tag,
                    int level, const char *dir) {
  std::vector<float> rgb;
  int w = 0, h = 0;
  if (!ReadTexture(fb, tex, rgb, w, h)) {
    Printf(PRINT_HIGH, "  %s L%d: <unavailable>\n", tag, level);
    return;
  }
  LevelStats s = ComputeStats(rgb, w, h, 0.05f);
  if (s.x1 < s.x0 || s.y1 < s.y0) {
    Printf(PRINT_HIGH, "  %s L%d %dx%d  peak=%.5f  <empty>\n", tag, level, w, h,
           s.maxLum);
    return;
  }
  const int bw = s.x1 - s.x0 + 1;
  const int bh = s.y1 - s.y0 + 1;
  Printf(PRINT_HIGH,
         "  %s L%d %4dx%-4d peak=%9.5f  bbox %3dx%-3d (%.2f:1)  px=%6d  energy=%.1f\n",
         tag, level, w, h, s.maxLum, bw, bh, bh > 0 ? (float)bw / bh : 0.0f,
         s.count, s.energy);

  FString path;
  path.Format("%s/bloomdump_%s_L%d.png", dir, tag, level);
  WritePNG(rgb, w, h, s.maxLum, path.GetChars());
}

// GetPPTexture CREATES the backing texture when it is absent. Before
// PPBloom::UpdateTextures has run, a level's PPTexture is still 0x0, and
// asking for it makes Metal assert on a zero-sized texture descriptor -- a
// crash caused purely by observing. So never ask unless the level has real
// dimensions. Debug code must not materialize the state it is inspecting.
static MTL::Texture *ReferenceLevelTexture(MetalRenderDevice *fb, int i) {
  auto texMan = fb->GetTextureManager();
  if (!texMan)
    return nullptr;
  PPTexture &vtex =
      const_cast<PPTexture &>(hw_postprocess.bloom.DebugLevel(i).VTexture);
  if (vtex.Width <= 0 || vtex.Height <= 0)
    return nullptr;
  return texMan->GetPPTexture(&vtex);
}

// Installed as PPBloom::DebugAfterExtract. Runs immediately after the
// reference extract draw and before any blur, which is the only moment that
// texture holds the extract -- the up-leg overwrites it later in the same
// RenderBloom call. MtPPRenderState::Draw ends its render pass
// (mt_postprocess.cpp:412), so encoding a blit here is safe, and encoding it
// (rather than reading now) is required: the draw itself has not executed yet.
static void CaptureInto(MTL::Texture *&slot, PPTexture *extractOutput) {
  MetalRenderDevice *fb = gDumpDevice;
  if (!fb || !extractOutput)
    return;
  auto texMan = fb->GetTextureManager();
  if (!texMan || extractOutput->Width <= 0 || extractOutput->Height <= 0)
    return;
  MTL::Texture *src = texMan->GetPPTexture(extractOutput);
  if (!src)
    return;

  if (slot && (slot->width() != src->width() ||
               slot->height() != src->height() ||
               slot->pixelFormat() != src->pixelFormat())) {
    slot->release();
    slot = nullptr;
  }
  if (!slot) {
    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(src->width());
    desc->setHeight(src->height());
    desc->setPixelFormat(src->pixelFormat());
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModePrivate);
    slot = fb->device->device->newTexture(desc);
    desc->release();
  }
  if (!slot)
    return;

  auto cmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
  if (!cmdBuf)
    return;
  fb->GetRenderState()->EndRenderPass();
  if (auto blit = cmdBuf->blitCommandEncoder()) {
    blit->copyFromTexture(src, 0, 0, MTL::Origin(0, 0, 0),
                          MTL::Size(src->width(), src->height(), 1),
                          slot, 0, 0, MTL::Origin(0, 0, 0));
    blit->endEncoding();
  }
}

static void CaptureReferenceExtract(PPTexture *out) {
  CaptureInto(gRefExtractSnapshot, out);
}

// Only level 0 matters here: the question is whether each half of a blur pair
// spreads along its own axis, and level 0 has the most resolution to show it.
static void CaptureReferenceBlur(PPTexture *out, bool vertical, int level) {
  if (level != 0)
    return;
  CaptureInto(vertical ? gRefBlurVSnapshot : gRefBlurHSnapshot, out);
}

} // namespace

void MtBloomDumpIfArmed(MetalRenderDevice *fb) {
  if (!gBloomDumpArmed || !fb)
    return;
  // Settle for a fixed number of frames rather than polling the textures for
  // content. Polling meant a blocking GPU readback every frame, which dragged
  // the frame rate down far enough that the countdown never elapsed -- the
  // measurement changing what it measures. A fixed delay costs nothing, and if
  // the levels really are empty the per-level output below says so plainly.
  if (--gBloomDumpFramesLeft > 0)
    return;
  gBloomDumpArmed = false;

  FString dir = M_GetDocumentsPath();
  dir += "bloomdump";
  CreatePath(dir.GetChars());

  Printf(PRINT_HIGH, "Bloom pyramid dump (previous frame's levels) -> %s\n",
         dir.GetChars());
  Printf(PRINT_HIGH,
         "  Extent is measured at 5%% of each level's OWN peak, so bbox aspect "
         "compares shape independent of gain.\n");
  Printf(PRINT_HIGH,
         "  Watch: at which level does the compute path's bbox HEIGHT first "
         "exceed the reference's?\n");

  if (fb->mBloomModule) {
    // The extract, before the pyramid. This is the row that separates an
    // extract divergence from a pyramid one -- the four levels below cannot,
    // because the up-leg leaves every one of them a copy of the deepest.
    DumpOne(fb, fb->mBloomModule->DebugExtractSnapshot(), "compute-extract", 0,
            dir.GetChars());
    for (int i = 0; i < MtBloomModule::DebugNumLevels; i++)
      DumpOne(fb, fb->mBloomModule->DebugLevel(i), "compute", i, dir.GetChars());
  }

  DumpOne(fb, gRefExtractSnapshot, "reference-extract", 0, dir.GetChars());
  DumpOne(fb, gRefBlurHSnapshot, "reference-afterH", 0, dir.GetChars());
  DumpOne(fb, gRefBlurVSnapshot, "reference-afterV", 0, dir.GetChars());
  // VTexture is the level's settled output on both legs -- the reference blurs
  // V->H then H->V, so V holds the result after each BlurStep pair.
  for (int i = 0; i < NumBloomLevels; i++)
    DumpOne(fb, ReferenceLevelTexture(fb, i), "reference", i, dir.GetChars());
}

// mt_bloom_dump [frames] -- dump after `frames` further rendered frames.
//
// The delay is an argument because the two ways of arming this need very
// different values. From the console, a handful of frames is right, but the
// console pauses the game and the world is not re-rendered while it is fully
// open, so the countdown only advances once it is closed. From the command
// line (+mt_bloom_dump 900) arming happens before the map or savegame has
// loaded, and the count has to outlast all of that.
CCMD(mt_bloom_dump) {
  gBloomDumpArmed = true;
  gBloomDumpFramesLeft = 30;
  gDumpDevice = ActiveMetalDevice();
  PPBloom::DebugAfterExtract = CaptureReferenceExtract;
  PPBloom::DebugAfterBlur = CaptureReferenceBlur;
  if (argv.argc() > 1) {
    int n = atoi(argv[1]);
    if (n > 0)
      gBloomDumpFramesLeft = n;
  }
  Printf(PRINT_HIGH, "  (dumping after %d further rendered frames -- close the "
                     "console, the world does not render while it is open)\n",
         gBloomDumpFramesLeft);
  Printf(PRINT_HIGH,
         "Bloom pyramid dump armed; it runs at the start of the next frame.\n"
         "Only the path that actually ran last frame holds fresh textures --\n"
         "run this once per configuration, one launch each.\n");
}
