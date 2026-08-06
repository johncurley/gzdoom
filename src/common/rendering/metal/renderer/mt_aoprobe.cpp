/*
** mt_aoprobe.cpp
** Runtime probe for the AO composite defect -- reports what the ssaocombine
** pass actually did, numerically, without a GPU frame capture.
**
**---------------------------------------------------------------------------
** Copyright 2026 John Curley
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
** Why this exists rather than reading a GPU frame capture:
**
** The capture route works -- mt_capture produces a valid trace -- but the four
** readings the AO investigation needs are buried in Xcode's viewer, and the
** trace's command stream is an undocumented binary format that cannot be
** parsed outside it. Meanwhile the engine already has everything needed to
** answer the same questions directly, and it can answer the most important one
** BETTER than a capture can.
**
** The decisive question is "did this draw change the render target at all".
** A capture answers it by eye, comparing attachment previews. This answers it
** by reading the target back before and after the draw and counting differing
** pixels -- which is exact, produces a number, and does not care whether the
** cause is stencil, blend or zero alpha.
*/

#ifdef __APPLE__

#include "../system/mt_renderdevice.h"
#include "../system/mt_commandbuffer.h"
#include "mt_renderbuffers.h"
#include "../textures/mt_texture.h"
#include "c_dispatch.h"
#include "printf.h"
#include "renderstyle.h"
#include <Metal/Metal.hpp>
#include <vector>
#include <cmath>

static bool gProbeArmed = false;
static int gProbeFramesLeft = 0;
// The frame's before-snapshot, held between the two hook points in
// MtPPRenderState::Draw.
static std::vector<float> gBefore;
static int gBeforeW = 0, gBeforeH = 0;
static bool gBeforeValid = false;

// Same readback as mt_bloomdump's, which is file-static there and cannot be
// shared. Handles the two formats SceneColor can have -- BGRA8Unorm, or
// RGBA16Float under mt_hdr_pipeline -- and SceneFog, which is always BGRA8.
static bool ReadTexture(MetalRenderDevice *fb, MTL::Texture *tex,
                        std::vector<float> &outRGB, int &w, int &h) {
  if (!tex)
    return false;

  const MTL::PixelFormat fmt = tex->pixelFormat();
  const bool halfFloat = fmt == MTL::PixelFormatRGBA16Float;
  const bool bgra8 = fmt == MTL::PixelFormatBGRA8Unorm;
  if (!halfFloat && !bgra8) {
    Printf(PRINT_HIGH, "  (probe: unhandled pixel format %d)\n", (int)fmt);
    return false;
  }

  w = (int)tex->width();
  h = (int)tex->height();
  const size_t bytesPerRow = (size_t)w * (halfFloat ? 8 : 4);
  const size_t dataSize = bytesPerRow * (size_t)h;

  // The postprocess passes before this one may still be sitting unencoded or
  // uncommitted. Without this the blit below reads whatever was in the texture
  // at the last flush, which would make the before/after comparison meaningless
  // in the most dangerous way -- it would look like "the draw did nothing".
  fb->GetCommands()->FlushCommands(true);

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

bool MtAOProbeArmed() { return gProbeArmed && gProbeFramesLeft <= 0; }

// True for the pass under investigation. Matched on the fragment shader lump
// so it cannot drift with group naming.
bool MtAOProbeWantsPass(const char *fragShaderName) {
  if (!MtAOProbeArmed() || !fragShaderName)
    return false;
  return strstr(fragShaderName, "ssaocombine") != nullptr;
}

void MtAOProbeCountdown() {
  if (gProbeArmed && gProbeFramesLeft > 0)
    gProbeFramesLeft--;
}

// READING 2, first half: the render target before the composite draw.
void MtAOProbeBefore(MetalRenderDevice *fb) {
  gBeforeValid = false;
  if (!fb || !fb->GetBuffers())
    return;
  gBeforeValid =
      ReadTexture(fb, fb->GetBuffers()->SceneColor->GetTexture(), gBefore,
                  gBeforeW, gBeforeH);
}

static const char *AlphaName(int v) {
  switch (v) {
  case STYLEALPHA_Zero: return "Zero";
  case STYLEALPHA_One: return "One";
  case STYLEALPHA_Src: return "SrcAlpha";
  case STYLEALPHA_InvSrc: return "InvSrcAlpha";
  default: return "other";
  }
}

// READINGS 1, 2, 3 and 4, reported together after the composite draw.
void MtAOProbeAfter(MetalRenderDevice *fb, const FRenderStyle &blend,
                    bool stencilTest, bool clearRequested) {
  if (!fb || !fb->GetBuffers())
    return;

  gProbeArmed = false;

  Printf(PRINT_HIGH,
         TEXTCOLOR_GOLD
         "\n=== AO composite probe -- ssaocombine ===\n" TEXTCOLOR_NORMAL);

  // READING 1. The blend style the runtime actually handed to PSO creation.
  // ConfigureBlendMode maps these deterministically, and the PP pipeline cache
  // key includes all three, so this is what the bound pipeline was built with.
  Printf(PRINT_HIGH, "[1] BlendMode  op=%d  src=%s(%d)  dst=%s(%d)\n",
         (int)blend.BlendOp, AlphaName(blend.SrcAlpha), (int)blend.SrcAlpha,
         AlphaName(blend.DestAlpha), (int)blend.DestAlpha);
  const bool alphaBlend = blend.BlendOp == STYLEOP_Add &&
                          blend.SrcAlpha == STYLEALPHA_Src &&
                          blend.DestAlpha == STYLEALPHA_InvSrc;
  const bool opaqueOverwrite = blend.BlendOp == STYLEOP_Add &&
                               blend.SrcAlpha == STYLEALPHA_One &&
                               blend.DestAlpha == STYLEALPHA_Zero;
  Printf(PRINT_HIGH, "    -> %s\n",
         alphaBlend ? "ALPHA BLEND, as expected for the normal path"
                    : opaqueOverwrite
                          ? "NO BLEND (One/Zero) -- this is the DEBUG path, so "
                            "gl_ssao_debug is not 0"
                          : "UNEXPECTED combination");

  // READING 3.
  Printf(PRINT_HIGH, "[3] stencilTest=%s  clearRequested=%s\n",
         stencilTest ? "ON" : "off", clearRequested ? "YES (Clear)" : "no (Load)");

  // READING 2. The decisive one, and exact rather than eyeballed.
  std::vector<float> after;
  int aw = 0, ah = 0;
  const bool afterValid =
      ReadTexture(fb, fb->GetBuffers()->SceneColor->GetTexture(), after, aw, ah);

  if (!gBeforeValid || !afterValid || aw != gBeforeW || ah != gBeforeH) {
    Printf(PRINT_HIGH, "[2] SceneColor before/after: UNAVAILABLE\n");
  } else {
    size_t differing = 0;
    double sumAbs = 0.0;
    float maxAbs = 0.0f;
    const size_t n = (size_t)aw * ah;
    for (size_t i = 0; i < n; i++) {
      float d = 0.0f;
      for (int c = 0; c < 3; c++)
        d = std::max(d, std::fabs(after[i * 3 + c] - gBefore[i * 3 + c]));
      if (d > 1.0f / 512.0f) {
        differing++;
        sumAbs += d;
      }
      maxAbs = std::max(maxAbs, d);
    }
    Printf(PRINT_HIGH,
           "[2] SceneColor %dx%d  differing px=%zu (%.2f%%)  maxdelta=%.5f  "
           "meandelta(of differing)=%.5f\n",
           aw, ah, differing, 100.0 * (double)differing / (double)n, maxAbs,
           differing ? sumAbs / (double)differing : 0.0);
    Printf(PRINT_HIGH, "    -> %s\n",
           differing == 0
               ? "THE DRAW CHANGED NOTHING. This is the defect, observed "
                 "directly."
               : "the draw DID modify SceneColor");
  }

  // READING 4. Sample SceneFog against SceneColor at the darkest pixels, which
  // is where AO should be strongest and where the dead ssaocombine patch would
  // matter most. Equal values there mean blending fogColor over an identical
  // destination is a no-op and the dead patch alone explains byte identity.
  std::vector<float> fog;
  int fw = 0, fh = 0;
  if (afterValid &&
      ReadTexture(fb, fb->GetBuffers()->SceneFog->GetTexture(), fog, fw, fh) &&
      fw == aw && fh == ah) {
    // Darkest 0.5% of pixels by luminance.
    std::vector<std::pair<float, size_t>> lum;
    lum.reserve((size_t)aw * ah);
    for (size_t i = 0; i < (size_t)aw * ah; i++) {
      float l = after[i * 3 + 0] * 0.299f + after[i * 3 + 1] * 0.587f +
                after[i * 3 + 2] * 0.114f;
      lum.push_back({l, i});
    }
    std::sort(lum.begin(), lum.end(),
              [](auto &a, auto &b) { return a.first < b.first; });
    const size_t sample = std::max<size_t>(1, lum.size() / 200);
    double meanFog = 0.0, meanCol = 0.0, meanAbsDiff = 0.0;
    size_t equalish = 0;
    for (size_t k = 0; k < sample; k++) {
      size_t i = lum[k].second;
      float fl = fog[i * 3 + 0] * 0.299f + fog[i * 3 + 1] * 0.587f +
                 fog[i * 3 + 2] * 0.114f;
      meanFog += fl;
      meanCol += lum[k].first;
      float d = std::fabs(fl - lum[k].first);
      meanAbsDiff += d;
      if (d < 1.0f / 512.0f)
        equalish++;
    }
    meanFog /= (double)sample;
    meanCol /= (double)sample;
    meanAbsDiff /= (double)sample;
    Printf(PRINT_HIGH,
           "[4] darkest %zu px:  SceneFog lum=%.5f  SceneColor lum=%.5f  "
           "mean|diff|=%.5f  equal=%zu/%zu\n",
           sample, meanFog, meanCol, meanAbsDiff, equalish, sample);
    Printf(PRINT_HIGH, "    -> %s\n",
           equalish > sample / 2
               ? "fogColor ~= destination here: the dead ssaocombine patch "
                 "ALONE can explain byte identity"
               : "fogColor differs from destination: the dead patch is NOT "
                 "sufficient, something else zeroes the contribution");
  } else {
    Printf(PRINT_HIGH, "[4] SceneFog comparison UNAVAILABLE\n");
  }

  Printf(PRINT_HIGH, TEXTCOLOR_GOLD "=== end probe ===\n\n" TEXTCOLOR_NORMAL);
  gBeforeValid = false;
}

// mt_ao_probe [frames] -- report on the ssaocombine pass, `frames` from now.
//
// Needs gl_ssao non-zero, or the pass does not run and nothing is reported.
// Set gl_ssao_debug 0 as well: any debug mode swaps the pass to SetNoBlend and
// reading [1] would come back "NO BLEND", which is correct behaviour rather
// than the defect.
CCMD(mt_ao_probe) {
  if (!screen || !screen->IsMetal()) {
    Printf(PRINT_HIGH, "mt_ao_probe: the Metal backend is not active.\n");
    return;
  }
  gProbeArmed = true;
  gProbeFramesLeft = 10;
  if (argv.argc() > 1) {
    int n = atoi(argv[1]);
    if (n > 0)
      gProbeFramesLeft = n;
  }
  Printf(PRINT_HIGH,
         "AO composite probe armed; reports after %d further rendered "
         "frames.\n",
         gProbeFramesLeft);
  Printf(PRINT_HIGH, TEXTCOLOR_YELLOW
         "  CLOSE THE CONSOLE" TEXTCOLOR_NORMAL
         " -- the world does not render while it is open.\n"
         "  Needs gl_ssao != 0. Keep gl_ssao_debug 0, or [1] reports the "
         "debug path's no-blend and that is not the defect.\n"
         "  It stalls the GPU three times for readbacks, so the frame it runs "
         "on will hitch. That is expected.\n");
}

#endif // __APPLE__
