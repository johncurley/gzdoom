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
  // Ambient0, the AO buffer the combine shader samples, is Rg16f: x is the
  // occlusion attenuation, y is the linear depth the shader's gate tests.
  const bool rg16 = fmt == MTL::PixelFormatRG16Float;
  if (!halfFloat && !bgra8 && !rg16) {
    Printf(PRINT_HIGH, "  (probe: unhandled pixel format %d)\n", (int)fmt);
    return false;
  }

  w = (int)tex->width();
  h = (int)tex->height();
  const size_t bytesPerRow = (size_t)w * (halfFloat ? 8 : (rg16 ? 4 : 4));
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
  if (rg16) {
    const __fp16 *pixels = (const __fp16 *)staging->contents();
    for (int i = 0; i < w * h; i++) {
      outRGB[i * 3 + 0] = (float)pixels[i * 2 + 0];
      outRGB[i * 3 + 1] = (float)pixels[i * 2 + 1];
      outRGB[i * 3 + 2] = 0.0f;
    }
  } else if (halfFloat) {
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
                    bool stencilTest, bool clearRequested,
                    MTL::Texture *aoInputTex) {
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

  // READING 4, REVISED. The first version of this sampled the darkest 0.5% of
  // SceneColor and compared luminance against SceneFog. That reading was
  // DEGENERATE: the darkest pixels are pure black, fog was black there too, and
  // "equal" was therefore guaranteed regardless of the truth. It answered a
  // question nobody asked -- blending black over black is a no-op whatever the
  // alpha. Whole-frame statistics instead, which cannot be gamed that way.
  std::vector<float> fog;
  int fw = 0, fh = 0;
  if (ReadTexture(fb, fb->GetBuffers()->SceneFog->GetTexture(), fog, fw, fh)) {
    double meanFog = 0.0;
    float maxFog = 0.0f;
    size_t nonBlack = 0;
    const size_t n = (size_t)fw * fh;
    for (size_t i = 0; i < n; i++) {
      float l = std::max(fog[i * 3 + 0], std::max(fog[i * 3 + 1], fog[i * 3 + 2]));
      meanFog += l;
      maxFog = std::max(maxFog, l);
      if (l > 1.0f / 512.0f)
        nonBlack++;
    }
    Printf(PRINT_HIGH,
           "[4] SceneFog %dx%d  mean=%.5f  max=%.5f  non-black px=%zu (%.2f%%)\n",
           fw, fh, meanFog / (double)n, maxFog, nonBlack,
           100.0 * (double)nonBlack / (double)n);
    Printf(PRINT_HIGH, "    -> %s\n",
           nonBlack == 0
               ? "fogColor is BLACK EVERYWHERE. The composite therefore blends "
                 "black, i.e. it DARKENS by alpha. Any non-zero alpha would "
                 "have shown in [2]."
               : "fog has content; the composite blends a non-black colour");
  } else {
    Printf(PRINT_HIGH, "[4] SceneFog UNAVAILABLE\n");
  }

  // READING 5. The alpha the shader actually computes, derived from the very
  // buffer it samples. This is what [2] being zero forces us to look at: with
  // blending correct, load correct, and fog black, the only way to change
  // nothing is for alpha to be zero. ssaocombine.fp computes
  //     depthMask = clamp(1 - exp2(-ssao.y * 0.01), 0, 1)
  //     alpha     = ssao.y > 2.0 ? (1 - ssao.x) * depthMask : 0
  std::vector<float> ao;
  int ax = 0, ay = 0;
  if (aoInputTex && ReadTexture(fb, aoInputTex, ao, ax, ay)) {
    const size_t n = (size_t)ax * ay;
    size_t gatePass = 0, alphaNonZero = 0;
    double meanY = 0.0, meanAtten = 0.0, meanAlpha = 0.0;
    float maxY = 0.0f, maxAlpha = 0.0f;
    for (size_t i = 0; i < n; i++) {
      const float atten = ao[i * 3 + 0];
      const float y = ao[i * 3 + 1];
      meanAtten += atten;
      meanY += y;
      maxY = std::max(maxY, y);
      float alpha = 0.0f;
      if (y > 2.0f) {
        gatePass++;
        const float depthMask =
            std::min(1.0f, std::max(0.0f, 1.0f - std::exp2(-y * 0.01f)));
        alpha = (1.0f - atten) * depthMask;
      }
      meanAlpha += alpha;
      maxAlpha = std::max(maxAlpha, alpha);
      if (alpha > 1.0f / 512.0f)
        alphaNonZero++;
    }
    Printf(PRINT_HIGH,
           "[5] AO input %dx%d  fmt=%d  mean ssao.x=%.5f  mean ssao.y=%.3f  "
           "max ssao.y=%.3f\n",
           ax, ay, (int)aoInputTex->pixelFormat(), meanAtten / (double)n,
           meanY / (double)n, maxY);
    Printf(PRINT_HIGH,
           "    gate ssao.y>2.0 passes: %zu/%zu (%.2f%%)   computed alpha: "
           "mean=%.5f max=%.5f  non-zero px=%zu (%.2f%%)\n",
           gatePass, n, 100.0 * (double)gatePass / (double)n,
           meanAlpha / (double)n, maxAlpha, alphaNonZero,
           100.0 * (double)alphaNonZero / (double)n);
    if (alphaNonZero == 0 && gatePass == 0)
      Printf(PRINT_HIGH,
             "    -> THE GATE FAILS EVERYWHERE. ssao.y is not what the shader "
             "expects here, so alpha is 0 and the blend is a no-op. The defect "
             "is UPSTREAM of the composite, in what reaches ssao.y.\n");
    else if (alphaNonZero == 0)
      Printf(PRINT_HIGH,
             "    -> gate passes but alpha is still 0: ssao.x is ~1 "
             "(no occlusion) everywhere.\n");
    else
      Printf(PRINT_HIGH,
             "    -> ALPHA IS NON-ZERO. The shader should be changing "
             "SceneColor. If [2] is zero, the fragments are not landing -- "
             "look at the stencil test.\n");
  } else {
    Printf(PRINT_HIGH, "[5] AO input texture UNAVAILABLE\n");
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
