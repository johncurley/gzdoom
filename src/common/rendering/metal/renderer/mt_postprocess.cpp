/*
**  Metal backend - Post-processing
**  Copyright (c) 2025 GZDoom Contributors
*/

#include "mt_postprocess.h"

void MtBloomDumpIfArmed(MetalRenderDevice *fb);
void MtWipeProbeIfArmed(MetalRenderDevice *fb);

#include "i_time.h"
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <chrono>

#include "c_cvars.h"
#include "printf.h"
#include "flatvertices.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "metal/renderer/mt_ao.h"
#include "metal/renderer/mt_bloom.h"
#include "metal/renderer/mt_debug.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "../utility/matrix.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/shaders/mt_shader.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_hwbuffer.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/textures/mt_sampler.h"
#include "metal/textures/mt_texture.h"
#include "r_videoscale.h"
#include "v_video.h"

// Defined in mt_aoprobe.cpp -- see there for why the AO composite is measured
// this way rather than read out of a GPU frame capture.
bool MtAOProbeWantsPass(const char *fragShaderName);
void MtAOProbeBefore(MetalRenderDevice *fb);
void MtAOProbeAfter(MetalRenderDevice *fb, const FRenderStyle &blend,
                    bool stencilTest, bool clearRequested,
                    MTL::Texture *aoInputTex);
void MtAOProbeCountdown();

EXTERN_CVAR(Int, gl_dither_bpc)
// Compute AO is OPT-IN on every platform. It used to default true, which meant
// the Intel guard below was the only thing keeping it off -- and on Apple Silicon,
// where that guard does not apply, it was therefore the DEFAULT on hardware nobody
// has ever run this backend on.
//
// That is not a defensible default given what the path does on the only hardware
// it has been measured on (2026-08-16, HD 6000, all in gameplay or size-matched
// captures):
//
//   quarter-res (the shipped Intel setting)  under-occludes ~20x -- effectively no AO
//   half-res, blur 2                         slower than reference, coarse
//                                            salt-and-pepper grain
//   half-res, blur 4 (the known grain fix)   CONSTANT FREEZING in gameplay
//
// The freezing is the reason this is opt-in rather than merely Intel-guarded. If it
// is a throughput problem, Apple Silicon erases it; if it is a synchronisation or
// hazard bug, it follows the code to Apple Silicon and lands there as the default.
// Nothing measured distinguishes those, and the benchmark harness reports the
// freezing configuration as healthy (avg 5.5ms, max 90.6ms, 4 stalls -- the same
// as the reference path), so it will not catch a regression here either.
//
// Being CVAR_ARCHIVE, anyone who has deliberately enabled it keeps it. Re-enable
// with `mt_compute_ao 1`. Validate with `mt_frametrace` while actually playing,
// not with the matrix suite -- see AGENTS.md.
CVAR(Bool, mt_compute_ao, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, mt_compute_bloom, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Use the compute bloom path on Intel integrated GPUs. Default false, matching
// mt_compute_ao_intel: on an HD 6000 a GPU frame capture on 2026-08-07 showed
// the compute encoders dominating GPU frame time, and disabling compute AO and
// bloom together restored playable frame pacing. Unlike the AO gate this is not
// backed by an isolated A/B of bloom alone -- it is the conservative default for
// a path measured as expensive on that hardware, not a precise cost figure.
//
// Non-Intel behaviour is unchanged (compute stays the default), for the same
// reason given at mt_compute_ao_intel: nothing on this path has run on Apple
// Silicon. Do not promote it to a stated policy without measuring.
CVAR(Bool, mt_compute_bloom_intel, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Diagnostic switch for the Metal-only PP stencil test on SceneColor targets.
// Not archived: this is for A/B runs, and an archived value silently surviving
// a restart is the exact trap that cost a session on 2026-08-07.
CVAR(Bool, mt_pp_stencil, true, 0)
CUSTOM_CVAR(Int, mt_compute_bloom_composite, 0, 0)
{
  if (self < 0) self = 0;
  if (self > 2) self = 2;
}
// AO resolution divisor. 1 = full-res (each AO sample is one screen pixel),
// 2 = half, 4 = quarter. The visible cost of a divisor > 1 is a block grid:
// one AO sample covers an NxN screen block, and the blur/atrous passes can
// only smooth *within* blocks, never remove the grid itself. That grid is
// the "salt and pepper squares" artifact. 1 is reachable but expensive;
// note the Intel budget override below, which may pin this higher.
CUSTOM_CVAR(Int, mt_compute_ao_scale, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 1) self = 1;
  if (self > 4) self = 4;
}
// Intel integrated GPUs get two hard quality overrides -- quarter-res AO and
// a clamp to 16 horizon samples -- from the 2026-07-14 cost bisection, which
// measured gl_ssao 3's 8x6=48 iterations at ~33ms of GPU frame time on an
// HD 6000 vs ~6.7ms at 4x4=16, against a ~17ms PP-AO-equivalent baseline.
// They exist so compute AO stays viable there rather than being off by
// default.
//
// They are overrides, not defaults, so before this CVAR there was no way to
// spend performance on quality on those GPUs no matter what you set. Set
// this to false to honour mt_compute_ao_scale and the sample-count CVARs as
// written. Expect a large frame-time cost: quarter -> half res alone is 4x
// the AO pixels, and lifting the sample clamp is another ~5x on top.
CVAR(Bool, mt_compute_ao_intel_clamp, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Use the compute AO path on Intel integrated GPUs. Default false: measured
// 2026-08-01 on an HD 6000, compute AO costs ~2x the reference PP path
// (22.05/22.41ms vs 11.42ms FrameGPU, a 1.95x ratio against a 0.36ms noise
// floor), which independently reproduces the 2026-07-14 bisection's ~1.9x.
// The machine is GPU-bound either way, so that is ~45fps vs ~87fps.
//
// Deliberately separate from mt_compute_ao_intel_clamp. Folding the two
// together would make the clamps unreachable -- anyone setting the clamp
// flag false to raise quality would simultaneously switch the path on, so
// the clamped configuration could never execute.
//
// Non-Intel behaviour is unchanged (compute stays the default). That is the
// status quo, NOT a measured decision: nothing on this path has ever run on
// Apple Silicon -- not the performance, not the Tier 2 read-write paths. Do
// not promote it to a stated policy without measuring.
CVAR(Bool, mt_compute_ao_intel, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, mt_compute_ao_normal_upsample, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, mt_compute_ao_normal_blur, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, mt_compute_ao_fullres_cleanup, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, mt_compute_ao_blur_passes, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 1) self = 1;
  if (self > 4) self = 4;
}
CVAR(Float, mt_compute_ao_combine_smooth, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, mt_compute_ao_skip_fullres, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, mt_compute_ao_atrous_passes, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0) self = 0;
  if (self > 3) self = 3;
}
CUSTOM_CVAR(Int, mt_compute_ao_steps, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0) self = 0;
  if (self > 16) self = 16;
}
CVAR(Float, mt_compute_ao_fade_start, 100.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, mt_compute_ao_fade_end, 500.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, mt_compute_ao_directions, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0) self = 0;
  if (self > 16) self = 16;
}
// 0 = GTAO (current, unchanged default), 1 = AlchemyAO/SAO, 2 = GTAO + depth-mip sampling
CUSTOM_CVAR(Int, mt_compute_ao_algorithm, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0) self = 0;
  if (self > 2) self = 2;
}
// 0 = use gl_ssao-tier default (8/8/12), same convention as mt_compute_ao_steps
CUSTOM_CVAR(Int, mt_compute_ao_alchemy_samples, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0) self = 0;
  if (self > 16) self = 16;
}
// Visual debug for the world-locked AO noise fix (see AGENTS.md). Renders
// full-screen, not a single spot-checked point, via the existing
// gl_ssao_debug 2 raw-AO grayscale display (NOT 1 -- gl_ssao_debug < 2
// still runs the bilateral blur pass on top of whatever the sample kernel
// wrote, which would smear this diagnostic; 2 skips it, showing the
// kernel's raw per-pixel output) -- each sample kernel writes a diagnostic
// value straight to aoOutput instead of running real AO math. 0 = normal
// (off). 1/2/3 = fract(worldPos.xy / cellSize), fract(worldPos.xz /
// cellSize), fract(worldPos.yz / cellSize) -- a correct world-locked
// pattern looks like a stable grid painted onto geometry: it must not
// slide when panning past a wall, and must not rotate/shift when turning
// in place near it.
CUSTOM_CVAR(Int, mt_compute_ao_worldpos_debug, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0) self = 0;
  if (self > 3) self = 3;
  // Announce the state on every change. This is CVAR_ARCHIVE, so it
  // survives across runs, and when it is on the AO buffer contains a
  // fract() grid rather than occlusion -- which is easy to mistake for a
  // broken AO pass, since it looks like severe banding/moire and responds
  // to camera motion. Several rounds of AO debugging were spent on
  // screenshots of this diagnostic before anyone noticed it was still
  // enabled from a previous session.
  if (self != 0)
    Printf(TEXTCOLOR_YELLOW "mt_compute_ao_worldpos_debug = %d: the AO buffer now shows the "
           "world-position grid, NOT ambient occlusion. Set it to 0 for real AO.\n", (int)self);
  else
    Printf("mt_compute_ao_worldpos_debug = 0: AO buffer shows real ambient occlusion.\n");
}
// World-space grid cell size (map units) the noise hash quantizes to --
// analogous to the old dither texture's implicit tiling frequency. Smaller
// = finer-grained dithering (closer to the old look) but coarser aliasing
// at distance; larger = coarser dithering but less distance aliasing.
// Default is a starting point for empirical tuning against the old look,
// not a measured value -- see AGENTS.md.
CUSTOM_CVAR(Float, mt_compute_ao_noise_cellsize, 12.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 1.0f) self = 1.0f;
}
// Target noise cell size in AO pixels. A fixed world-space cell size can't
// serve both ends of the depth range -- one cell is one noise sample, so a
// 12-unit cell is dozens of pixels wide up close (every pixel in it gets
// the same jitter, giving the large correlated AO blotches that crawl
// diagonally across surfaces as the player walks, since the cell grid is
// world-anchored and the camera isn't) and sub-pixel at distance. This
// scales the cell with view depth so it stays ~this many AO pixels wide
// everywhere; see NoiseCellSize in mt_ao.metal. 1.0 is the natural value
// (one noise sample per AO pixel, what the blur pass is tuned for); raise
// it for chunkier/cheaper-looking dither. 0 disables the adaptive path and
// restores the fixed mt_compute_ao_noise_cellsize behaviour.
CUSTOM_CVAR(Float, mt_compute_ao_noise_pixels, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0.0f) self = 0.0f;
  if (self > 16.0f) self = 16.0f;
}
// How much screen-space interleaved-gradient noise is mixed into the
// world-cell jitter. Needed because world-cell noise cannot decorrelate
// pixels on grazing surfaces at any cell size -- a world cube projects to
// a long thin screen run there, and every pixel in it marches identically,
// which is the dark-streak-near-close-geometry artifact. See AoNoise in
// mt_ao.metal for the full geometry. The term is a pure function of pixel
// coordinate (no frame counter), so it is static in screen space and
// cannot shimmer or crawl. 0 = pure world-locked noise (restores the
// streaks); 1 = full decorrelation.
CUSTOM_CVAR(Float, mt_compute_ao_noise_screenmix, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0.0f) self = 0.0f;
  if (self > 1.0f) self = 1.0f;
}
// Thickness reject threshold in map units, previously a hard-coded 1.25f
// in MtAOModule::Render() with no way to sweep it. The shader scales this
// linearly with view depth (mt_ao.metal:381-383); it is NOT tied to the
// horizon step distance or to N.V surface slant the way Jimenez et al.
// 2016 describes, which is why it is suspected in the grazing-angle
// streaks. Set it to something huge (e.g. 1000) to effectively disable the
// reject and see whether the artifact is thickness-related at all.
CUSTOM_CVAR(Float, mt_compute_ao_thickness, 1.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0.01f) self = 0.01f;
}

class MtPPRenderState : public PPRenderState {
public:
  MtPPRenderState(MetalRenderDevice *fb) : fb(fb) {}
  MTL::Texture *customOutputTex = nullptr;

  // The group name is recorded rather than pushed as a Metal debug group,
  // because PushGroup is called before this pass's render command encoder
  // exists -- BeginRenderPass happens inside Draw(). Draw() applies it as the
  // encoder's LABEL instead, which is what Xcode's frame navigator lists each
  // pass by, so a capture reads as "PP ssao: shaders/pp/ssaocombine.fp"
  // rather than as a flat run of anonymous draws. These were empty stubs until
  // 2026-08-06; the AO composite investigation needed a navigable trace.
  FString mGroupName;

  void PushGroup(const FString &name) override { mGroupName = name; }

  void PopGroup() override { mGroupName = ""; }

  void Draw() override {
    if (!fb->GetBuffers()) return;

    // AO composite probe (mt_ao_probe). Snapshot the render target before the
    // pass under investigation, so the draw's actual contribution can be
    // measured rather than eyeballed in a frame capture. Both hooks sit
    // outside the render encoder's lifetime, which a readback requires.
    const bool aoProbe =
        Shader && MtAOProbeWantsPass(Shader->FragmentShader.GetChars());
    MTL::Texture *aoProbeInputTex = nullptr;
    if (aoProbe)
      MtAOProbeBefore(fb);
    auto renderState = fb->GetRenderState();
    auto mtRenderState = static_cast<MtRenderState *>(renderState);

    // Determine output target
    MTL::Texture *outputTex = nullptr;
    int width = fb->GetBuffers()->GetWidth();
    int height = fb->GetBuffers()->GetHeight();
    MTL::PixelFormat format = MTL::PixelFormatBGRA8Unorm;
    MTL::Texture *depthStencil = nullptr;
    bool stencilTest = false;

    if (customOutputTex) {
      outputTex = customOutputTex;
      width = (int)outputTex->width();
      height = (int)outputTex->height();
      format = outputTex->pixelFormat();
    } else if (Output.Type == PPTextureType::SwapChain) {
      outputTex = nullptr; // use default/swapchain
      // Match the swapchain format (usually BGRA8Unorm) instead of forcing
      // RGBA16Float
      if (fb->mCurrentDrawable) {
          format = (MTL::PixelFormat)fb->mCurrentDrawable->texture()->pixelFormat();
          width = (int)fb->mCurrentDrawable->texture()->width();
          height = (int)fb->mCurrentDrawable->texture()->height();
      } else {
          format = MTL::PixelFormatBGRA8Unorm;
          width = fb->GetClientWidth();
          height = fb->GetClientHeight();
      }
    } else if (Output.Type == PPTextureType::PPTexture) {
      outputTex = fb->GetTextureManager()->GetPPTexture(Output.Texture);
      if (outputTex) {
        width = (int)outputTex->width();
        height = (int)outputTex->height();
        format = outputTex->pixelFormat();
      }
    } else if (Output.Type == PPTextureType::CurrentPipelineTexture) {
      outputTex =
          fb->GetBuffers()
              ->PipelineImage[fb->GetPostprocess()->mCurrentPipelineImage]
              ->GetTexture();
      format = (MTL::PixelFormat)fb->GetBuffers()->GetPipelineFormat();
    } else if (Output.Type == PPTextureType::NextPipelineTexture) {
      int next = (fb->GetPostprocess()->mCurrentPipelineImage + 1) %
                 MtRenderBuffers::NumPipelineImages;
      outputTex = fb->GetBuffers()->PipelineImage[next]->GetTexture();
      format = (MTL::PixelFormat)fb->GetBuffers()->GetPipelineFormat();
    } else if (Output.Type == PPTextureType::SceneColor) {
      outputTex = fb->GetBuffers()->SceneColor->GetTexture();
      format = (MTL::PixelFormat)fb->GetBuffers()->GetSceneColorFormat();
      // SSAO Fix: Enable Stencil Test when targeting SceneColor to avoid bleeding through portals
      depthStencil = fb->GetBuffers()->SceneDepthStencil->GetTexture();
      // DIAGNOSTIC (2026-08-08): this stencil test is Metal-only -- the shared
      // hw_postprocess AO combine has no stencil concept at all, so GL writes
      // AO everywhere. Metal renders spurious occlusion below ~row 360 of 768
      // on AshesHardReset MAP01 where GL is clean. Set mt_pp_stencil 0 to take
      // the GL-equivalent path and isolate whether this test is responsible.
      stencilTest = mt_pp_stencil;
    } else if (Output.Type == PPTextureType::SceneFog) {
      outputTex = fb->GetBuffers()->SceneFog->GetTexture();
      format = (MTL::PixelFormat)fb->GetBuffers()->GetSceneFogFormat();
    } else if (Output.Type == PPTextureType::SceneNormal) {
      outputTex = fb->GetBuffers()->SceneNormal->GetTexture();
      format = (MTL::PixelFormat)fb->GetBuffers()->GetSceneNormalFormat();
    } else if (Output.Type == PPTextureType::SceneDepth) {
      outputTex = fb->GetBuffers()->SceneDepthStencil->GetTexture();
      format = MTL::PixelFormatDepth32Float_Stencil8;
    } else if (Output.Type == PPTextureType::ShadowMap) {
      outputTex = fb->GetBuffers()->ShadowMap->GetTexture();
      format = MTL::PixelFormatR32Float;
    }

    if (outputTex) {
      width = (int)outputTex->width();
      height = (int)outputTex->height();
      format = outputTex->pixelFormat();
    }

    mtRenderState->SetRenderTarget(outputTex, depthStencil, width, height,
                                   (int)format, 1);
    // Ensure PP pass uses a single color attachment
    mtRenderState->EnableDrawBuffers(1, false);

    // Explicitly set the viewport for the PP pass
    mtRenderState->SetViewport(Viewport.left, Viewport.top, Viewport.width,
                               Viewport.height);
    mtRenderState->SetScissor(0, 0, width, height);

    // Handle clearing if requested
    const bool clearRequested =
        BlendMode.SrcAlpha == (uint8_t)STYLEALPHA_One &&
        BlendMode.DestAlpha == (uint8_t)STYLEALPHA_Zero && !ShadowMapBuffers;
    if (clearRequested) {
      mtRenderState->Clear(CT_Color);
    }

    mtRenderState->BeginRenderPass();
    auto encoder = mtRenderState->GetEncoder();
    if (!encoder) {
      return;
    }

    MtShaderProgram *program = fb->GetShaderManager()->GetPPShader(Shader);
    if (!program || !program->vert || !program->frag) {
      return;
    }

    // Name the pass for GPU frame captures. Costs a string build per PP draw,
    // which is nothing beside the draw itself, and turns an unreadable trace
    // into one where the pass under investigation can be found by name.
    if (Shader) {
      FString label;
      if (mGroupName.IsNotEmpty())
        label.Format("PP %s: %s", mGroupName.GetChars(),
                     Shader->FragmentShader.GetChars());
      else
        label.Format("PP %s", Shader->FragmentShader.GetChars());
      encoder->setLabel(NS::String::string(
          label.GetChars(), NS::StringEncoding::UTF8StringEncoding));
    }

    auto pipeline = fb->GetPipelineStateManager()->GetPPPipelineState(
        program, (MTL::PixelFormat)format, BlendMode,
        depthStencil ? depthStencil->pixelFormat() : MTL::PixelFormatInvalid,
        stencilTest);
    if (pipeline) {
      mtRenderState->SetVertexBuffer(screen->mVertexData);
      auto vb = dynamic_cast<MtVertexBuffer *>(screen->mVertexData->GetBufferObjects().first);
      if (vb) {
        MtPipelineKey ppKey;
        ppKey.VertexFormat = vb->VertexFormat;
        mtRenderState->SetPipelineKey(ppKey);

        encoder->setRenderPipelineState(pipeline);
        if (stencilTest) {
            encoder->setDepthStencilState(fb->GetPipelineStateManager()->GetPPStencilState());
            encoder->setStencilReferenceValue(screen->stencilValue);
        } else {
            encoder->setDepthStencilState(fb->GetPipelineStateManager()->GetDisabledDepthStencilState());
        }
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setVertexBuffer(vb->GetBuffer(), 0, 0);
        encoder->useResource(vb->GetBuffer(), MTL::ResourceUsageRead, MTL::RenderStageVertex);
      }

      for (int i = 0; i < (int)Textures.Size(); ++i) {
        auto &input = Textures[i];
        MTL::Texture *tex = nullptr;

        switch (input.Type) {
        case PPTextureType::CurrentPipelineTexture:
          tex = fb->GetBuffers()
                    ->PipelineImage[fb->GetPostprocess()->mCurrentPipelineImage]
                    ->GetTexture();
          break;
        case PPTextureType::NextPipelineTexture: {
          int next = (fb->GetPostprocess()->mCurrentPipelineImage + 1) %
                     MtRenderBuffers::NumPipelineImages;
          tex = fb->GetBuffers()->PipelineImage[next]->GetTexture();
          break;
        }
        case PPTextureType::PPTexture:
          tex = fb->GetTextureManager()->GetPPTexture(input.Texture);
          break;
        case PPTextureType::SceneColor:
          tex = fb->GetBuffers()->SceneColor->GetTexture();
          break;
        case PPTextureType::SceneDepth:
          tex = fb->GetBuffers()->SceneDepthStencil->GetTexture();
          break;
        case PPTextureType::SceneFog:
          tex = fb->GetBuffers()->SceneFog->GetTexture();
          break;
        case PPTextureType::SceneNormal:
          tex = fb->GetBuffers()->SceneNormal->GetTexture();
          break;
        default:
          break;
        }

        if (tex) {
          // Input 0 of ssaocombine is Ambient0, the AO buffer whose contents
          // decide the alpha. The probe derives the shader's own alpha from it.
          if (aoProbe && i == 0)
            aoProbeInputTex = tex;
          encoder->setFragmentTexture(tex, i);

          // Set sampler state based on input filter/wrap modes
          MtSamplerKey samplerKey;
          samplerKey.MinFilter = (input.Filter == PPFilterMode::Linear) ? 1 : 0;
          samplerKey.MagFilter = (input.Filter == PPFilterMode::Linear) ? 1 : 0;
          samplerKey.MipFilter = 0;
          samplerKey.AddressU = (input.Wrap == PPWrapMode::Repeat)
                                    ? 0
                                    : 3; // 0=Repeat, 3=ClampToEdge (CLAMP_XY)
          samplerKey.AddressV = (input.Wrap == PPWrapMode::Repeat) ? 0 : 3;
          samplerKey.AddressW = (input.Wrap == PPWrapMode::Repeat) ? 0 : 3;
          samplerKey.MaxAnisotropy = 1;

          auto sampler = fb->GetSamplerManager()->GetSamplerState(samplerKey);
          if (sampler) {
            encoder->setFragmentSamplerState(sampler, i);
          }
        }
      }

      // Bind uniforms
      if (Uniforms.Data.Size() > 0) {
        encoder->setFragmentBytes(Uniforms.Data.Data(), Uniforms.Data.Size(),
                                  0);
      }

      // Draw quad (1 triangle covering the screen)
      encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                              (NS::UInteger)FFlatVertexBuffer::PRESENT_INDEX,
                              (NS::UInteger)3);
    }

    mtRenderState->EndRenderPass();

    // Read the target back now the pass is closed and report all four
    // readings. Disarms itself, so this costs one frame's hitch, once.
    if (aoProbe)
      MtAOProbeAfter(fb, BlendMode, stencilTest, clearRequested,
                     aoProbeInputTex);

    // Advance pipeline index if output was Next and no custom output was used
    if (Output.Type == PPTextureType::NextPipelineTexture && !customOutputTex) {
      fb->GetPostprocess()->mCurrentPipelineImage =
          (fb->GetPostprocess()->mCurrentPipelineImage + 1) %
          MtRenderBuffers::NumPipelineImages;
    }
  }

private:
  MetalRenderDevice *fb;
};

MtPostprocess::MtPostprocess(MetalRenderDevice *fb) : fb(fb) {}
MtPostprocess::~MtPostprocess() {}

void MtPostprocess::BlurScene(float amount) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  MtPPRenderState renderstate(fb);
  hw_postprocess.bloom.RenderBlur(&renderstate, sceneWidth, sceneHeight, amount);
}

void MtPostprocess::AmbientOccludeScene(float m5, const HWViewpointUniforms* currentViewpoint) {
  if (!fb->mAOModule) return;

  // Use the scene size, as VkPostprocess::AmbientOccludeScene does. This used
  // to override both with SceneDepthStencil's texture size, which feeds
  // AmbientWidth/Height and hence RadiusToScreen -- i.e. the AO radius in
  // pixels. Measured equal (1024x820 both) in the aobug repro, so this is an
  // alignment with the reference backend, not a fix for anything observed;
  // it only matters if the depth texture is ever padded past the scene.
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  // Set Z planes from the last scene viewpoint for correct linearization
  fb->mZNear = fb->mLastSceneViewpoint.mProjectionMatrix.get()[14] / (fb->mLastSceneViewpoint.mProjectionMatrix.get()[10] - 1.0f);
  fb->mZFar = fb->mLastSceneViewpoint.mProjectionMatrix.get()[14] / (fb->mLastSceneViewpoint.mProjectionMatrix.get()[10] + 1.0f);

  if (fb->mZNear < 0.1f) fb->mZNear = 5.0f;
  if (fb->mZFar < fb->mZNear) fb->mZFar = 65536.0f;

  // Intel integrated GPUs default to the reference PP path -- compute AO
  // measured at ~2x its cost there. See mt_compute_ao_intel.
  bool useComputeAO = mt_compute_ao;
  if (useComputeAO && !mt_compute_ao_intel &&
      fb->mVersionManager.architecture == MtGPUArchitecture::Intel) {
    useComputeAO = false;
  }

  if (useComputeAO && fb->mAOModule->Render(m5, sceneWidth, sceneHeight, currentViewpoint)) {
    RestoreSceneRenderTargetAfterAO();
    return;
  }

  // Use the renderer's postprocess SSAO path (matches GL/Vulkan) so depth
  // linearization and blending are handled the same way as the reference
  // backends.
  MtPPRenderState renderstate(fb);
  auto aoStart = std::chrono::high_resolution_clock::now();
  hw_postprocess.ssao.Render(&renderstate, m5, sceneWidth, sceneHeight);
  auto aoEnd = std::chrono::high_resolution_clock::now();
  if (fb->GetDebugManager()) {
    float ms = std::chrono::duration<float, std::milli>(aoEnd - aoStart).count();
    fb->GetDebugManager()->RecordMetric(MtMetric::PPAO, ms);
  }

  RestoreSceneRenderTargetAfterAO();
}

// AmbientOccludeScene runs in the MIDDLE of the scene -- hw_drawinfo.cpp:1071,
// after the opaque pass and before portals and translucents -- and unlike
// Vulkan, whose VkPPRenderState is a separate object, Metal's MtPPRenderState
// drives the *main* render state. Every PP pass therefore points that state at
// a postprocess target and forces a single colour attachment
// (MtPPRenderState::Draw, EnableDrawBuffers(1, false)), and nothing put it back.
//
// The consequence was silent and expensive: MtPipelineStateManager selects the
// shader variant with `DrawBufferCount > 1 ? GBUFFER_PASS : NORMAL_PASS`, and
// FragNormal only exists under GBUFFER_PASS, so every surface drawn after the
// AO pass was compiled against a shader with no normal output. Colour and depth
// still landed in attachment 0, so nothing looked wrong -- but the scene normal
// G-buffer stayed empty (134 of 135 wall draws, measured), which is what broke
// SSAO on Metal.
void MtPostprocess::RestoreSceneRenderTargetAfterAO() {
  auto *renderState = fb->GetRenderState();
  SetSceneRenderTarget(gl_ssao != 0);
  renderState->EnableDrawBuffers(renderState->GetPassDrawBufferCount(), false);
}

void MtPostprocess::ClearTonemapPalette() {
  hw_postprocess.tonemap.ClearTonemapPalette();
}

void MtPostprocess::UpdateShadowMap() {
  if (screen->mShadowMap.PerformUpdate()) {
    MtPPRenderState renderstate(fb);
    hw_postprocess.shadowmap.Update(&renderstate);
    screen->mShadowMap.FinishUpdate();
  }
}

void MtPostprocess::SetActiveRenderTarget() {
  auto buffers = fb->GetBuffers();
  auto tex = buffers->PipelineImage[mCurrentPipelineImage]->GetTexture();
  fb->GetRenderState()->SetRenderTarget(
      tex, buffers->PipelineDepthStencil->GetTexture(), buffers->GetWidth(),
      buffers->GetHeight(), buffers->GetPipelineFormat(), 1);
  // Ensure this active postprocess target uses a single color attachment
  fb->GetRenderState()->EnableDrawBuffers(1, false);
  fb->GetRenderState()->SetViewport(0, 0, fb->GetWidth(), fb->GetHeight());
}

void MtPostprocess::PostProcessScene(
    bool swscene, int fixedcm, float flash,
    const std::function<void()> &afterBloomDrawEndScene2D) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  // Sample SceneColor before Pass1 moves it into the pipeline images, so the
  // HDR probe sees raw scene output rather than postprocessed colour or the
  // 2D pass. No-op unless mt_hdr_probe armed it.
  if (auto debug = fb->GetDebugManager())
    debug->CaptureHdrProbe();

  // Runs before anything this frame overwrites the pyramid, and reads the
  // levels the previous frame left behind -- see mt_bloomdump.cpp on why it
  // cannot read this frame's.
  MtBloomDumpIfArmed(fb);
  MtWipeProbeIfArmed(fb);
  MtAOProbeCountdown();

  MtPPRenderState renderstate(fb);

  if (!swscene) {
    const bool bloomEligible = gl_bloom && fixedcm == CM_DEFAULT &&
                               gl_ssao_debug == 0 &&
                               sceneWidth > 0 && sceneHeight > 0;
    // Intel integrated GPUs default to the reference PP bloom path, mirroring
    // the compute AO gate above. See mt_compute_bloom_intel.
    const bool computeBloomAllowed =
        mt_compute_bloom &&
        (mt_compute_bloom_intel ||
         fb->mVersionManager.architecture != MtGPUArchitecture::Intel);
    const bool useComputeBloom = bloomEligible && computeBloomAllowed && fb->mBloomModule;

    hw_postprocess.Pass1(&renderstate, fixedcm, sceneWidth, sceneHeight, bloomEligible);

    if (bloomEligible) {
      bool computeBloomRendered = false;
      if (useComputeBloom) {
        auto cmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
        auto srcTex = fb->GetBuffers()->PipelineImage[mCurrentPipelineImage]->GetTexture();
        if (cmdBuf && srcTex) {
          fb->GetRenderState()->EndRenderPass();
          // hw_postprocess.Pass1 above ran exposure.Render, so CameraTexture
          // holds this frame's adaptation. The reference extract multiplies
          // by it; without it compute bloom is visibly dimmer in dark scenes.
          MTL::Texture *exposureTex = fb->GetTextureManager()->GetPPTexture(
              &hw_postprocess.exposure.CameraTexture);
          computeBloomRendered = fb->mBloomModule->Execute(cmdBuf, srcTex, gl_bloom_amount,
                                                          exposureTex);
        }
      }

      if (!computeBloomRendered) {
        auto bloomStart = std::chrono::high_resolution_clock::now();
        hw_postprocess.bloom.RenderBloom(&renderstate, sceneWidth, sceneHeight, fixedcm);
        auto bloomEnd = std::chrono::high_resolution_clock::now();
        if (fb->GetDebugManager()) {
          float ms = std::chrono::duration<float, std::milli>(bloomEnd - bloomStart).count();
          fb->GetDebugManager()->RecordMetric(MtMetric::PPBloom, ms);
        }
      }
    }

    SetActiveRenderTarget();
    afterBloomDrawEndScene2D();
    hw_postprocess.Pass2(&renderstate, fixedcm, flash, sceneWidth, sceneHeight);
  } else {
    // Software scene post-processing path if needed
    afterBloomDrawEndScene2D();
  }
}

void MtPostprocess::ImageTransitionScene(bool undefinedSrcLayout) {
  // Metal doesn't need explicit transitions
}

void MtPostprocess::BlitSceneToPostprocess() {
  fb->GetRenderState()->EndRenderPass();

  auto buffers = fb->GetBuffers();
  mCurrentPipelineImage = 0;

  // Use a render pass to copy & scale SceneColor into the pipeline image (handles differing sizes)
  auto dst = buffers->PipelineImage[0]->GetTexture();
  if (!dst)
    return;

  MtPPRenderState renderstate(fb);
  // Render directly into the pipeline image (customOutputTex is honored by Draw())
  renderstate.customOutputTex = dst;
  renderstate.Clear();
  renderstate.Shader = &hw_postprocess.present.Present;
  PresentUniforms uniforms;
  uniforms.InvGamma = 1.0f;
  uniforms.Contrast = 1.0f;
  uniforms.Brightness = 0.0f;
  uniforms.Saturation = 1.0f;
  uniforms.GrayFormula = 0;
  uniforms.ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);
  // Map the scene viewport into pipeline image space using SceneScale/SceneOffset
  {
    auto sceneScale = screen->SceneScale();
    auto sceneOffset = screen->SceneOffset();
    uniforms.Scale = { sceneScale.X, sceneScale.Y };
    uniforms.Offset = { sceneOffset.X, sceneOffset.Y };
  }
  uniforms.HdrMode = 0;
  renderstate.Uniforms.Set(uniforms);
  renderstate.Viewport = { 0, 0, fb->GetWidth(), fb->GetHeight() };
  renderstate.SetInputSceneColor(0, PPFilterMode::Linear);
  // Bind dither texture (present shader expects sampler at index 1)
  renderstate.SetInputTexture(1, &hw_postprocess.present.Dither, PPFilterMode::Nearest, PPWrapMode::Repeat);
  renderstate.SetNoBlend();
  renderstate.Draw();

  // Mark destination as filled so future passes don't clear it
  auto mtRenderState = static_cast<MtRenderState *>(fb->RenderState());
  mtRenderState->MarkAsFilled(dst);
}

void MtPostprocess::BlitCurrentToImage(MTL::Texture *dstimage) {
  fb->GetRenderState()->EndRenderPass();
  fb->GetCommands()->FlushCommands(true);

  auto srcimage =
      fb->GetBuffers()->PipelineImage[mCurrentPipelineImage]->GetTexture();

  if (!srcimage || !dstimage)
    return;

  // If formats match, use blit encoder
  if (srcimage->pixelFormat() == dstimage->pixelFormat()) {
    auto blitCmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
    auto blitEncoder = blitCmdBuf->blitCommandEncoder();
    blitEncoder->copyFromTexture(srcimage, dstimage);

    auto mtRenderState = static_cast<MtRenderState *>(fb->RenderState());
    mtRenderState->MarkAsFilled(dstimage);

    blitEncoder->endEncoding();

    // Safety: For these specific blits (usually for wipes or screenshots),
    // wait for completion to avoid flashes of uninitialized data.
    blitCmdBuf->commit();
    blitCmdBuf->waitUntilCompleted();
    blitCmdBuf->release();
  } else {
    // Use a simple draw call to convert formats.
    // Reaching here is unexpected: the only caller matches the pipeline
    // format deliberately. Log it, because this path's orientation handling
    // has never been exercised.
    Printf(PRINT_LOG,
           "Metal: BlitCurrentToImage format-conversion path taken (src %d -> dst %d). "
           "This path is untested; verify orientation.\n",
           (int)srcimage->pixelFormat(), (int)dstimage->pixelFormat());
    MtPPRenderState renderstate(fb);
    renderstate.customOutputTex = dstimage;
    renderstate.Clear();
    renderstate.Shader =
        &hw_postprocess.present.Present; // Use present shader for simple blit
    PresentUniforms uniforms;
    uniforms.InvGamma = 1.0f;
    uniforms.Contrast = 1.0f;
    uniforms.Brightness = 0.0f;
    uniforms.Saturation = 1.0f;
    uniforms.GrayFormula = 0;
    uniforms.ColorScale = 255.0f;
    // Flip V so this path matches the orientation of the copyFromTexture
    // path above. Without it the function's output orientation depends on
    // whether the caller's formats happen to agree, which shipped once as an
    // upside-down screen wipe (dce604c31) when mt_hdr_pipeline made the
    // pipeline images RGBA16Float while CreateWipeTexture still asked for
    // BGRA8Unorm. present.fp samples at TexCoord * UVScale + UVOffset.
    //
    // VERIFIED 2026-08-07 by mt_wipe_probe, which blits the same pipeline
    // image down both legs and correlates their per-row luminance profiles:
    //   r(convert, copy) = +0.9986   r(convert, copy REVERSED) = +0.0967
    // and the falsification half, with this flip removed, reports the exact
    // mirror on the SAME frame (+0.0967 / +0.9986, VERDICT INVERTED). Both
    // halves were needed:
    // the AGREE result alone is also what a probe that cannot detect inversion
    // would print.
    //
    // The flip is correct because the present shader is excluded from
    // PatchVertexShader's PP V-flip (mt_shader.cpp), so it carries an IMPLICIT
    // flip on Metal -- the same one the chain used to survive by cancellation.
    // This explicit flip is the second one, and the two cancel, matching the
    // copyFromTexture leg above.
    //
    // The branch still has no reachable caller: CreateWipeTexture is the only
    // caller of BlitCurrentToImage and always matches the pipeline format.
    // Re-run `mt_wipe_probe` after touching either leg.
    uniforms.Scale = {1.0f, -1.0f};
    uniforms.Offset = {0.0f, 1.0f};
    uniforms.HdrMode = 0;
    renderstate.Uniforms.Set(uniforms);
    renderstate.Uniforms.Set(uniforms);
    renderstate.Viewport = {0, 0, (int)dstimage->width(),
                            (int)dstimage->height()};
    renderstate.SetInputCurrent(0, PPFilterMode::Linear);
    renderstate.SetInputTexture(1, &hw_postprocess.present.Dither,
                                PPFilterMode::Nearest, PPWrapMode::Repeat);

    // Determine if dstimage is the swapchain
    bool isSwap = false;
    if (fb->mCurrentDrawable &&
        (MTL::Texture *)fb->mCurrentDrawable->texture() == dstimage) {
      isSwap = true;
    }

    if (isSwap) {
      renderstate.SetOutputSwapChain();
    } else {
      // For internal images (wipes, screenshots), we use customOutputTex
      // No call to SetOutput... is needed as NextPipelineTexture is the default
      // but Draw() will prioritize customOutputTex if set.
    }

    renderstate.SetNoBlend();
    renderstate.Draw();
  }
  if (srcimage->pixelFormat() != dstimage->pixelFormat())
    fb->GetCommands()->FlushCommands(true);
}

void MtPostprocess::DrawPresentTexture(IntRect box, bool applyGamma,
                                       bool screenshot) {
  MtPPRenderState renderstate(fb);

  // The "screen" custom-shader target runs here, matching Vulkan
  // (vk_postprocess.cpp:190) and OpenGL (gl_postprocess.cpp:150). Metal never
  // ran it at all: GLDEFS accepts `HardwareShader postprocess screen`, the
  // shader parses, lists and enables, and then nothing executes it, because
  // Pass1/Pass2 in shared code only dispatch "beforebloom" and "scene".
  //
  // The screenshot guard is the reference's and the reason is the reference's:
  // GetScreenshotBuffer is called after the swap, so the pass has already been
  // applied to the frame being copied and running it again would double it.
  if (!screenshot)
    hw_postprocess.customShaders.Run(&renderstate, "screen");

  PresentUniforms uniforms;

  if (!applyGamma) {
    uniforms.InvGamma = 1.0f;
    uniforms.Contrast = 1.0f;
    uniforms.Brightness = 0.0f;
    uniforms.Saturation = 1.0f;
    uniforms.GrayFormula = 0;
  } else {
    uniforms.InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
    uniforms.Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
    uniforms.Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
    uniforms.Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
    uniforms.GrayFormula = static_cast<int>(gl_satformula);
  }
  uniforms.ColorScale =
      (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);

  // Final orientation correction for presentation.
  // Flips the synchronized internal frame to be right-side up for the screen.
  uniforms.Scale = {1.0f, -1.0f};
  uniforms.Offset = {0.0f, 1.0f};

  uniforms.HdrMode = 0;

  renderstate.Clear();
  renderstate.Shader = &hw_postprocess.present.Present;
  renderstate.Uniforms.Set(uniforms);
  renderstate.Viewport = box;
  renderstate.SetInputCurrent(0, PPFilterMode::Linear);
  renderstate.SetInputTexture(1, &hw_postprocess.present.Dither,
                              PPFilterMode::Nearest, PPWrapMode::Repeat);

  if (screenshot)
    renderstate.SetOutputNext();
  else {
    renderstate.SetOutputSwapChain();
    if (fb->mCurrentDrawable) {
      static_cast<MtRenderState *>(fb->GetRenderState())
          ->MarkAsFilled((MTL::Texture *)fb->mCurrentDrawable->texture());
    }
  }

  renderstate.SetNoBlend();
  renderstate.Draw();
}

MTL::Texture *MtPostprocess::GetCurrentTexture() {
  auto buffers = fb->GetBuffers();
  if (!buffers || !buffers->PipelineImage[mCurrentPipelineImage])
    return nullptr;
  return buffers->PipelineImage[mCurrentPipelineImage]->GetTexture();
}

void MtPostprocess::SetSceneRenderTarget(bool useSSAO) {
  fb->GetRenderState()->SetRenderTarget(
      fb->GetBuffers()->SceneColor->GetTexture(),
      fb->GetBuffers()->SceneDepthStencil->GetTexture(),
      fb->GetBuffers()->GetSceneWidth(),
      fb->GetBuffers()->GetSceneHeight(),
      fb->GetBuffers()->GetSceneColorFormat(),
      fb->GetBuffers()->GetSceneSamples());
}
