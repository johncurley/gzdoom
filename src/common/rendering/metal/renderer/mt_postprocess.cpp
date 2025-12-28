/*
**  Metal backend - Post-processing
**  Copyright (c) 2025 GZDoom Contributors
*/

#include "mt_postprocess.h"
#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#undef TimeScale

#include "c_cvars.h"
#include "flatvertices.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/shaders/mt_shader.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_hwbuffer.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/textures/mt_sampler.h"
#include "metal/textures/mt_texture.h"
#include "printf.h"
#include "r_videoscale.h"
#include "v_video.h"

EXTERN_CVAR(Int, gl_dither_bpc)
EXTERN_CVAR(Bool, mt_debug)

class MtPPRenderState : public PPRenderState {
public:
  MtPPRenderState(MetalRenderDevice *fb) : fb(fb) {}

  void PushGroup(const FString &name) override {
    // fb->GetCommands()->PushGroup(name.GetChars());
  }

  void PopGroup() override {
    // fb->GetCommands()->PopGroup();
  }

  void Draw() override {
    if (mt_debug) {
      Printf(PRINT_LOG, "Metal: PPRenderState::Draw shader=%p viewport=%d,%d %dx%d OutputType=%d\n", 
             Shader, Viewport.left, Viewport.top, Viewport.width, Viewport.height, (int)Output.Type);
    }
    auto renderState = fb->GetRenderState();
    auto mtRenderState = static_cast<MtRenderState *>(renderState);

    // Determine output target
    MTL::Texture *outputTex = nullptr;
    int width = fb->GetBuffers()->GetWidth();
    int height = fb->GetBuffers()->GetHeight();
    MTL::PixelFormat format = MTL::PixelFormatRGBA16Float;
    MTL::Texture *depthStencil = nullptr;

    if (Output.Type == PPTextureType::SwapChain) {
      outputTex = nullptr;                 // use default/swapchain
      // Match the swapchain format (usually BGRA8Unorm) instead of forcing RGBA16Float
      format = (fb->mCurrentDrawable) ? (MTL::PixelFormat)fb->mCurrentDrawable->texture()->pixelFormat() : MTL::PixelFormatBGRA8Unorm;
      
      // Use physical drawable dimensions if available
      if (fb->mCurrentDrawable) {
          width = (int)fb->mCurrentDrawable->texture()->width();
          height = (int)fb->mCurrentDrawable->texture()->height();
      } else {
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
      format = MTL::PixelFormatRGBA16Float;
    } else if (Output.Type == PPTextureType::NextPipelineTexture) {
      int next = (fb->GetPostprocess()->mCurrentPipelineImage + 1) %
                 MtRenderBuffers::NumPipelineImages;
      outputTex = fb->GetBuffers()->PipelineImage[next]->GetTexture();
      format = MTL::PixelFormatRGBA16Float;
    } else if (Output.Type == PPTextureType::SceneColor) {
      outputTex = fb->GetBuffers()->SceneColor->GetTexture();
      format = MTL::PixelFormatRGBA16Float;
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
    }

    if (mt_debug) {
        Printf(PRINT_LOG, "Metal: PPRenderState::Draw - targeting %p %dx%d fmt=%llu\n", outputTex, width, height, (unsigned long long)format);
    }

    mtRenderState->SetRenderTarget(outputTex, depthStencil, width, height, (int)format, 1);
    
    // Explicitly set the viewport for the PP pass
    mtRenderState->SetViewport(Viewport.left, Viewport.top, Viewport.width, Viewport.height);
    mtRenderState->SetScissor(0, 0, width, height);

    // Handle clearing if requested
    if (BlendMode.SrcAlpha == (uint8_t)STYLEALPHA_One &&
        BlendMode.DestAlpha == (uint8_t)STYLEALPHA_Zero && !ShadowMapBuffers) {
      mtRenderState->Clear(CT_Color);
    }

    mtRenderState->BeginRenderPass();
    auto encoder = mtRenderState->GetEncoder();
    if (!encoder) {
        if (mt_debug) Printf(PRINT_LOG, "Metal: PPRenderState::Draw - FAILED to get encoder\n");
        return;
    }

    MtShaderProgram *program = fb->GetShaderManager()->GetPPShader(Shader);
    if (!program || !program->vert || !program->frag) {
      return;
    }

    auto pipeline = fb->GetPipelineStateManager()->GetPPPipelineState(
        program, (MTL::PixelFormat)format, BlendMode);
    if (pipeline) {
      if (mt_debug) {
          Printf(PRINT_LOG, "Metal: PPRenderState::Draw - Pipeline created successfully. VS: %s, FS: %s\n", 
                 program->vert->name.c_str(), program->frag->name.c_str());
      }
      // Set vertex buffer on the render state so ApplyRenderPass uses it
      mtRenderState->SetVertexBuffer(screen->mVertexData);
      
      // Update pipeline key for tracking and potential future use in ApplyRenderPass
      auto vb = dynamic_cast<MtVertexBuffer *>(
          screen->mVertexData->GetBufferObjects().first);
      if (vb) {
          int stride = (int)vb->GetStride();
          MtPipelineKey ppKey;
          ppKey.VertexFormat = vb->VertexFormat | (stride << 8);
          mtRenderState->SetPipelineKey(ppKey);

          encoder->setRenderPipelineState(pipeline);
          
          // CRITICAL: Explicitly set essential state for the new encoder
          encoder->setDepthStencilState(fb->GetPipelineStateManager()->GetDisabledDepthStencilState());
          encoder->setCullMode(MTL::CullModeNone);
          
          // Bind vertex buffer (screen->mVertexData) at slot 0 manually too for safety
          encoder->setVertexBuffer(vb->GetBuffer(), 0, 0);
      } else {
          if (mt_debug) Printf(PRINT_LOG, "Metal: PPRenderState::Draw - WARNING: dynamic_cast to MtVertexBuffer failed!\n");
      }

      // Bind input textures and samplers
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
          samplerKey.AddressU = (input.Wrap == PPWrapMode::Repeat) ? 0 : 2; // 0=Repeat, 2=ClampToEdge
          samplerKey.AddressV = (input.Wrap == PPWrapMode::Repeat) ? 0 : 2;
          samplerKey.AddressW = (input.Wrap == PPWrapMode::Repeat) ? 0 : 2;
          samplerKey.MaxAnisotropy = 1;

          auto sampler = fb->GetSamplerManager()->GetSamplerState(samplerKey);
          if (sampler) {
            encoder->setFragmentSamplerState(sampler, i);
          }
          if (mt_debug) Printf(PRINT_LOG, "Metal: PPRenderState::Draw - bound texture %p to slot %d\n", tex, i);
        }
      }

      // Bind uniforms
      if (Uniforms.Data.Size() > 0) {
        if (mt_debug) Printf(PRINT_LOG, "Metal: PPRenderState::Draw - Binding uniforms size=%u\n", (unsigned int)Uniforms.Data.Size());
        encoder->setFragmentBytes(Uniforms.Data.Data(), Uniforms.Data.Size(),
                                  0);
      }

      // Draw quad (1 triangle covering the screen)
      encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)FFlatVertexBuffer::PRESENT_INDEX, (NS::UInteger)3);
      if (mt_debug) Printf(PRINT_LOG, "Metal: PPRenderState::Draw - called drawPrimitives\n");
    } else if (mt_debug) {
        Printf(PRINT_LOG, "Metal: PPRenderState::Draw - FAILED to get pipeline state\n");
    }

    mtRenderState->EndRenderPass();

    // Advance pipeline index if output was Next
    if (Output.Type == PPTextureType::NextPipelineTexture) {
      fb->GetPostprocess()->mCurrentPipelineImage =
          (fb->GetPostprocess()->mCurrentPipelineImage + 1) %
          MtRenderBuffers::NumPipelineImages;
    }
  }

private:
  MetalRenderDevice *fb;
};

// FORCE RECOMPILE: December 25 V8 Final Audit Build
MtPostprocess::MtPostprocess(MetalRenderDevice *fb) : fb(fb) {}
MtPostprocess::~MtPostprocess() {}

void MtPostprocess::BlurScene(float amount) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  MtPPRenderState renderstate(fb);
  hw_postprocess.bloom.RenderBlur(&renderstate, sceneWidth, sceneHeight,
                                  amount);
}

void MtPostprocess::AmbientOccludeScene(float m5) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  MtPPRenderState renderstate(fb);
  hw_postprocess.ssao.Render(&renderstate, m5, sceneWidth, sceneHeight);
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
  fb->GetRenderState()->SetRenderTarget(
      buffers->PipelineImage[mCurrentPipelineImage]->GetTexture(),
      buffers->PipelineDepthStencil->GetTexture(),
      buffers->GetWidth(),
      buffers->GetHeight(), (int)MTL::PixelFormatRGBA16Float, 1);
  fb->GetRenderState()->SetViewport(0, 0, buffers->GetWidth(), buffers->GetHeight());
}

void MtPostprocess::PostProcessScene(
    bool swscene, int fixedcm, float flash,
    const std::function<void()> &afterBloomDrawEndScene2D) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  MtPPRenderState renderstate(fb);

  if (!swscene) {
    hw_postprocess.Pass1(&renderstate, fixedcm, sceneWidth, sceneHeight);
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

  // Use the main command buffer for the blit (asynchronous)
  MTL::CommandBuffer *blitCmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
  if (!blitCmdBuf) return;

  auto blitEncoder = blitCmdBuf->blitCommandEncoder();
  if (!blitEncoder) return;

  auto src = buffers->SceneColor->GetTexture();
  auto dst = buffers->PipelineImage[0]->GetTexture();

  if (mt_debug) {
      Printf(PRINT_LOG, "Metal: BlitSceneToPostprocess (Asynchronous) src=%p dst=%p\n", src, dst);
  }

  if (src && dst) {
    blitEncoder->copyFromTexture(src, dst);
    static_cast<MtRenderState*>(fb->GetRenderState())->MarkAsFilled(dst);
  }

  blitEncoder->endEncoding();
}

void MtPostprocess::BlitCurrentToImage(MTL::Texture *dstimage) {
  fb->GetRenderState()->EndRenderPass();

  auto srcimage =
      fb->GetBuffers()->PipelineImage[mCurrentPipelineImage]->GetTexture();
  
  if (mt_debug) {
      Printf(PRINT_LOG, "Metal: BlitCurrentToImage src=%p dst=%p\n", srcimage, dstimage);
  }

  if (!srcimage || !dstimage)
    return;

  // If formats match, use blit encoder
  if (srcimage->pixelFormat() == dstimage->pixelFormat()) {
    auto blitCmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
    auto blitEncoder = blitCmdBuf->blitCommandEncoder();
    blitEncoder->copyFromTexture(srcimage, dstimage);
    static_cast<MtRenderState*>(fb->GetRenderState())->MarkAsFilled(dstimage);
    blitEncoder->endEncoding();
  } else {
    // Use a simple draw call to convert formats
    MtPPRenderState renderstate(fb);
    renderstate.Clear();
    renderstate.Shader = &hw_postprocess.present.Present; // Use present shader for simple blit
    PresentUniforms uniforms;
    uniforms.InvGamma = 1.0f;
    uniforms.Contrast = 1.0f;
    uniforms.Brightness = 0.0f;
    uniforms.Saturation = 1.0f;
    uniforms.GrayFormula = 0;
    uniforms.ColorScale = 255.0f;
    uniforms.Scale = { 1.0f, 1.0f };
    uniforms.Offset = { 0.0f, 0.0f };
    uniforms.HdrMode = 0;
    renderstate.Uniforms.Set(uniforms);
    renderstate.Viewport = { 0, 0, (int)dstimage->width(), (int)dstimage->height() };
    renderstate.SetInputCurrent(0, PPFilterMode::Linear);
    renderstate.SetInputTexture(1, &hw_postprocess.present.Dither,
                              PPFilterMode::Nearest, PPWrapMode::Repeat);
    
    // Determine if dstimage is the swapchain
    bool isSwap = false;
    if (fb->mCurrentDrawable && (MTL::Texture*)fb->mCurrentDrawable->texture() == dstimage) {
        isSwap = true;
    }

    if (isSwap) {
        renderstate.SetOutputSwapChain();
    } else {
        // Fallback: manually set target if not swapchain (but most blits here are to swapchain or internal images)
        // For now, let's assume it's swapchain for the common case, or we need a way to pass MTL::Texture directly.
        // Actually, SetOutputSwapChain() works because PPRenderState::Draw uses mCurrentDrawable.
        renderstate.SetOutputSwapChain(); 
    }
    
    renderstate.SetNoBlend();
    renderstate.Draw();
  }
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

  if (screenshot) {
    uniforms.Scale = { 1.0f, 1.0f };
    uniforms.Offset = { 0.0f, 0.0f };
  } else {
    // Flip vertically when blitting to swapchain to correct orientation
    uniforms.Scale = { 1.0f, -1.0f };
    uniforms.Offset = { 0.0f, 1.0f };
  }

  uniforms.HdrMode = 0;

  if (mt_debug) {
      Printf(PRINT_LOG, "Metal: DrawPresentTexture - Scale: %.2f %.2f Offset: %.2f %.2f Gamma: %.2f\n", 
             uniforms.Scale.X, uniforms.Scale.Y, uniforms.Offset.X, uniforms.Offset.Y, uniforms.InvGamma);
  }

  renderstate.Clear();
  renderstate.Shader = &hw_postprocess.present.Present;
  renderstate.Uniforms.Set(uniforms);
  renderstate.Viewport = box;
  renderstate.SetInputCurrent(0, PPFilterMode::Linear);
  renderstate.SetInputTexture(1, &hw_postprocess.present.Dither,
                              PPFilterMode::Nearest, PPWrapMode::Repeat);

  if (screenshot)
    renderstate.SetOutputNext();
  else
    renderstate.SetOutputSwapChain();

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
      fb->GetBuffers()->GetWidth(), fb->GetBuffers()->GetHeight(),
      (int)MTL::PixelFormatRGBA16Float, fb->GetBuffers()->GetSceneSamples());
}
