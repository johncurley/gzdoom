/*
**  Metal backend - Post-processing
**  Copyright (c) 2025 GZDoom Contributors
*/

#include "mt_postprocess.h"
#include "i_time.h"
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <chrono>

#include "c_cvars.h"
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

EXTERN_CVAR(Int, gl_dither_bpc)
CVAR(Bool, mt_compute_ao, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, mt_compute_bloom, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, mt_compute_ao_scale, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 2) self = 2;
  if (self > 4) self = 4;
}
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
CUSTOM_CVAR(Int, mt_compute_ao_directions, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
  if (self < 0) self = 0;
  if (self > 16) self = 16;
}

class MtPPRenderState : public PPRenderState {
public:
  MtPPRenderState(MetalRenderDevice *fb) : fb(fb) {}
  MTL::Texture *customOutputTex = nullptr;

  void PushGroup(const FString &name) override {
    // fb->GetCommands()->PushGroup(name.GetChars());
  }

  void PopGroup() override {
    // fb->GetCommands()->PopGroup();
  }

  void Draw() override {
    if (!fb->GetBuffers()) return;
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
      format = MTL::PixelFormatBGRA8Unorm;
    } else if (Output.Type == PPTextureType::NextPipelineTexture) {
      int next = (fb->GetPostprocess()->mCurrentPipelineImage + 1) %
                 MtRenderBuffers::NumPipelineImages;
      outputTex = fb->GetBuffers()->PipelineImage[next]->GetTexture();
      format = MTL::PixelFormatBGRA8Unorm;
    } else if (Output.Type == PPTextureType::SceneColor) {
      outputTex = fb->GetBuffers()->SceneColor->GetTexture();
      format = MTL::PixelFormatBGRA8Unorm;
      // SSAO Fix: Enable Stencil Test when targeting SceneColor to avoid bleeding through portals
      depthStencil = fb->GetBuffers()->SceneDepthStencil->GetTexture();
      stencilTest = true;
    } else if (Output.Type == PPTextureType::SceneFog) {
      outputTex = fb->GetBuffers()->SceneFog->GetTexture();
      format = MTL::PixelFormatBGRA8Unorm;
    } else if (Output.Type == PPTextureType::SceneNormal) {
      outputTex = fb->GetBuffers()->SceneNormal->GetTexture();
      format = MTL::PixelFormatBGRA8Unorm;
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
    if (BlendMode.SrcAlpha == (uint8_t)STYLEALPHA_One &&
        BlendMode.DestAlpha == (uint8_t)STYLEALPHA_Zero && !ShadowMapBuffers) {
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

void MtPostprocess::AmbientOccludeScene(float m5) {
  if (!fb->mAOModule) return;

  auto depthTex = fb->GetBuffers()->SceneDepthStencil.get();
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();
  if (depthTex) {
    sceneWidth = depthTex->GetWidth();
    sceneHeight = depthTex->GetHeight();
  }

  // Set Z planes from the last scene viewpoint for correct linearization
  fb->mZNear = fb->mLastSceneViewpoint.mProjectionMatrix.get()[14] / (fb->mLastSceneViewpoint.mProjectionMatrix.get()[10] - 1.0f);
  fb->mZFar = fb->mLastSceneViewpoint.mProjectionMatrix.get()[14] / (fb->mLastSceneViewpoint.mProjectionMatrix.get()[10] + 1.0f);

  if (fb->mZNear < 0.1f) fb->mZNear = 5.0f;
  if (fb->mZFar < fb->mZNear) fb->mZFar = 65536.0f;

  if (mt_compute_ao && fb->mAOModule->Render(m5, sceneWidth, sceneHeight)) {
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
      buffers->GetHeight(), (int)MTL::PixelFormatBGRA8Unorm, 1);
  // Ensure this active postprocess target uses a single color attachment
  fb->GetRenderState()->EnableDrawBuffers(1, false);
  fb->GetRenderState()->SetViewport(0, 0, fb->GetWidth(), fb->GetHeight());
}

void MtPostprocess::PostProcessScene(
    bool swscene, int fixedcm, float flash,
    const std::function<void()> &afterBloomDrawEndScene2D) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  MtPPRenderState renderstate(fb);

  if (!swscene) {
    const bool bloomEligible = gl_bloom && fixedcm == CM_DEFAULT &&
                               gl_ssao_debug == 0 &&
                               sceneWidth > 0 && sceneHeight > 0;
    const bool useComputeBloom = bloomEligible && mt_compute_bloom && fb->mBloomModule;

    hw_postprocess.Pass1(&renderstate, fixedcm, sceneWidth, sceneHeight, bloomEligible);

    if (bloomEligible) {
      bool computeBloomRendered = false;
      if (useComputeBloom) {
        auto cmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
        auto srcTex = fb->GetBuffers()->PipelineImage[mCurrentPipelineImage]->GetTexture();
        if (cmdBuf && srcTex) {
          fb->GetRenderState()->EndRenderPass();
          computeBloomRendered = fb->mBloomModule->Execute(cmdBuf, srcTex, gl_bloom_amount);
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
    // Use a simple draw call to convert formats
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
    uniforms.Scale = {1.0f, 1.0f};
    uniforms.Offset = {0.0f, 0.0f};
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
      (int)MTL::PixelFormatBGRA8Unorm, fb->GetBuffers()->GetSceneSamples());
}
